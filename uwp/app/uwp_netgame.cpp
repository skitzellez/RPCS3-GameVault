// Copyright (c) 2026 Skitzelles - part of RPCS3 for Game Vault (github.com/skitzellez/RPCS3-GameVault), GPL-2.0.
//
// RPCS3 for Game Vault: a PS3 game played straight from a PC's shared folder or a NAS (Game Vault's network games),
// with no copy on the console first. Apps on the Xbox cannot open \\PC\share paths, so this speaks SMB2/3 itself
// through libsmb2 (smb2.dll, LGPL-2.1, the same build Game Vault and Xenia for Game Vault use, loaded only when a
// network game starts). The share is an RPCS3 virtual device: every fs:: call on its paths comes here, so a game
// folder (PS3_DISC.SFB, PS3_GAME/...) and a .iso (through RPCS3's own ISO reader on top) both work.
//
// Reads go through small caches of 1 MB pieces per file, so the game's many small reads don't each cost a network
// round trip; a dropped connection (a PC that slept, Wi-Fi that blinked) is opened again once by itself.

#include "stdafx.h"
#include "uwp_netgame.h"

#include "Utilities/File.h"

#include <winsock2.h>
#include <windows.h>
#include <fcntl.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

LOG_CHANNEL(net_log, "NetGame");

namespace uwp_netgame
{
	namespace
	{
		// libsmb2 (include/smb2/libsmb2.h): the few calls used; the library is loaded at run time
		struct smb2_context;
		struct smb2fh;
		struct smb2dir;
		struct smb2_stat_64
		{
			u32 smb2_type;
			u32 smb2_nlink;
			u64 smb2_ino;
			u64 smb2_size;
			u64 times[8]; // atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec, btime, btime_nsec
		};
		struct smb2dirent
		{
			const char* name;
			smb2_stat_64 st;
		};
		constexpr u32 c_type_directory = 1; // SMB2_TYPE_DIRECTORY
		constexpr u16 c_signing_enabled = 0x0001; // SMB2_NEGOTIATE_SIGNING_ENABLED

		struct smb2_api
		{
			smb2_context* (*init_context)();
			void (*destroy_context)(smb2_context*);
			void (*set_timeout)(smb2_context*, int);
			void (*set_security_mode)(smb2_context*, u16);
			void (*set_domain)(smb2_context*, const char*);
			void (*set_password)(smb2_context*, const char*);
			int (*connect_share)(smb2_context*, const char*, const char*, const char*);
			int (*disconnect_share)(smb2_context*);
			smb2fh* (*open)(smb2_context*, const char*, int);
			int (*close)(smb2_context*, smb2fh*);
			int (*pread)(smb2_context*, smb2fh*, u8*, u32, u64);
			u32 (*get_max_read_size)(smb2_context*);
			const char* (*get_error)(smb2_context*);
			smb2dir* (*opendir)(smb2_context*, const char*);
			smb2dirent* (*readdir)(smb2_context*, smb2dir*);
			void (*closedir)(smb2_context*, smb2dir*);
			int (*stat)(smb2_context*, const char*, smb2_stat_64*);
		};

		const smb2_api* load_smb2()
		{
			static smb2_api api;
			static std::once_flag once;
			static bool ok = false;
			std::call_once(once, []()
			{
				HMODULE m = LoadPackagedLibrary(L"smb2.dll", 0);
				if (!m)
				{
					net_log.error("smb2.dll could not be loaded (%u)", GetLastError());
					return;
				}
				auto get = [&](auto& fn, const char* name)
				{
					fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
					return fn != nullptr;
				};
				ok = get(api.init_context, "smb2_init_context") && get(api.destroy_context, "smb2_destroy_context") &&
					get(api.set_timeout, "smb2_set_timeout") && get(api.set_security_mode, "smb2_set_security_mode") &&
					get(api.set_domain, "smb2_set_domain") && get(api.set_password, "smb2_set_password") &&
					get(api.connect_share, "smb2_connect_share") && get(api.disconnect_share, "smb2_disconnect_share") &&
					get(api.open, "smb2_open") && get(api.close, "smb2_close") && get(api.pread, "smb2_pread") &&
					get(api.get_max_read_size, "smb2_get_max_read_size") && get(api.get_error, "smb2_get_error") &&
					get(api.opendir, "smb2_opendir") && get(api.readdir, "smb2_readdir") &&
					get(api.closedir, "smb2_closedir") && get(api.stat, "smb2_stat");
				if (!ok)
				{
					net_log.error("smb2.dll is missing a function");
					return;
				}
				WSADATA wsa;
				WSAStartup(MAKEWORD(2, 2), &wsa);
			});
			return ok ? &api : nullptr;
		}

		void wipe(std::string& s)
		{
			std::fill(s.begin(), s.end(), ' ');
			s.clear();
		}

		struct cred
		{
			std::string server, share, user = "Guest", password;
		};

		// the sign-in for server\share from Game Vault's file; the file is gone from the disk at once
		cred read_cred(const std::string& file, const std::string& server, const std::string& share)
		{
			cred found{server, share};
			if (file.empty())
				return found;
			std::string text;
			if (fs::file f{file})
			{
				text = f.to_string();
				f.close();
				fs::write_file(file, fs::rewrite, std::string(text.size(), ' '));
			}
			fs::remove_file(file);
			usz at = 0;
			while (at < text.size())
			{
				usz end = text.find('\n', at);
				if (end == umax)
					end = text.size();
				std::string line = text.substr(at, end - at);
				at = end + 1;
				while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
					line.pop_back();
				std::vector<std::string> f;
				usz pos = 0;
				for (int i = 0; i < 3; i++)
				{
					const usz tab = line.find('\t', pos);
					if (tab == umax)
						break;
					f.push_back(line.substr(pos, tab - pos));
					pos = tab + 1;
				}
				f.push_back(line.substr(pos));
				if (f.size() == 4 && !_stricmp(f[0].c_str(), server.c_str()) && !_stricmp(f[1].c_str(), share.c_str()))
				{
					found.user = f[2].empty() ? "Guest" : f[2];
					found.password = f[3];
				}
				for (auto& s : f)
					wipe(s);
				wipe(line);
			}
			wipe(text);
			return found;
		}

		s64 smb_time(u64 seconds) { return static_cast<s64>(seconds); }

		// One connection to the share for the whole game; libsmb2 contexts are not thread-safe, so one lock
		class nas_device final : public fs::device_base
		{
		public:
			static constexpr std::string_view c_name = "game_vault_nas_dev";
			static constexpr std::string_view c_prefix = "/vfsv0_virtual_game_vault_nas_dev";

			nas_device(const smb2_api* api, cred c, std::string base)
				: m_api(api), m_cred(std::move(c)), m_base(std::move(base))
			{
				fs_prefix = std::string(c_prefix);
			}

			~nas_device() override
			{
				drop();
				wipe(m_cred.password);
			}

			bool connect()
			{
				std::lock_guard lock(m_mutex);
				return connect_locked();
			}

			// "<prefix>/PS3_GAME/USRDIR/x" -> "base\PS3_GAME\USRDIR\x" (libsmb2 sends the path as it is)
			std::string full(std::string_view path) const
			{
				if (path.starts_with(c_prefix))
					path.remove_prefix(c_prefix.size());
				while (!path.empty() && (path.front() == '/' || path.front() == '\\'))
					path.remove_prefix(1);
				while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
					path.remove_suffix(1);
				std::string rel(path);
				if (rel == ".")
					rel.clear();
				std::replace(rel.begin(), rel.end(), '/', '\\');
				if (rel.empty())
					return m_base;
				return m_base.empty() ? rel : m_base + "\\" + rel;
			}

			bool stat_full(const std::string& p, smb2_stat_64& st)
			{
				std::lock_guard lock(m_mutex);
				if (m_ctx && m_api->stat(m_ctx, p.c_str(), &st) >= 0)
					return true;
				// a name that isn't there fails too: once more on a fresh connection only when the connection is gone
				if (!m_ctx && connect_locked() && m_api->stat(m_ctx, p.c_str(), &st) >= 0)
					return true;
				return false;
			}

			bool stat(const std::string& path, fs::stat_t& info) override
			{
				smb2_stat_64 st{};
				if (!stat_full(full(path), st))
				{
					fs::g_tls_error = fs::error::noent;
					return false;
				}
				info = fs::stat_t{};
				info.is_directory = st.smb2_type == c_type_directory;
				info.is_writable = false;
				info.size = info.is_directory ? 0 : st.smb2_size;
				info.atime = smb_time(st.times[0]);
				info.mtime = smb_time(st.times[2]);
				info.ctime = smb_time(st.times[4]);
				return true;
			}

			bool statfs(const std::string& path, fs::device_stat& info) override
			{
				fs::stat_t st;
				if (!stat(path, st))
					return false;
				info = fs::device_stat{.block_size = 4096, .total_size = st.size, .total_free = 0, .avail_free = 0};
				return true;
			}

			// read-only: the share is the player's library
			bool remove_dir(const std::string&) override { fs::g_tls_error = fs::error::acces; return false; }
			bool create_dir(const std::string&) override { fs::g_tls_error = fs::error::acces; return false; }
			bool rename(const std::string&, const std::string&) override { fs::g_tls_error = fs::error::acces; return false; }
			bool remove(const std::string&) override { fs::g_tls_error = fs::error::acces; return false; }
			bool trunc(const std::string&, u64) override { fs::g_tls_error = fs::error::acces; return false; }

			std::unique_ptr<fs::file_base> open(const std::string& path, bs_t<fs::open_mode> mode) override;
			std::unique_ptr<fs::dir_base> open_dir(const std::string& path) override;

			// Reads a file through *fh, opened (again) when it isn't open on the current connection
			bool read_at(const std::string& p, smb2fh** fh, u64* gen, u64 at, u8* buf, usz len)
			{
				std::lock_guard lock(m_mutex);
				for (int attempt = 0; attempt < 2; attempt++)
				{
					if (attempt)
					{
						net_log.warning("read of %s at 0x%x failed, connecting again", p, at);
						if (!connect_locked())
							return false;
					}
					if (!m_ctx)
						continue;
					if (!*fh || *gen != m_generation)
					{
						*fh = m_api->open(m_ctx, p.c_str(), O_RDONLY);
						*gen = m_generation;
						if (!*fh)
						{
							net_log.error("%s could not be opened: %s", p, m_api->get_error(m_ctx));
							continue;
						}
					}
					usz done = 0;
					while (done < len)
					{
						const u32 n = static_cast<u32>(std::min<usz>(len - done, m_max_read));
						const int got = m_api->pread(m_ctx, *fh, buf + done, n, at + done);
						if (got <= 0)
							break;
						done += static_cast<usz>(got);
					}
					if (done == len)
						return true;
				}
				return false;
			}

			void close(smb2fh* fh, u64 gen)
			{
				std::lock_guard lock(m_mutex);
				if (fh && m_ctx && gen == m_generation)
					m_api->close(m_ctx, fh);
			}

			bool list(const std::string& p, std::vector<fs::dir_entry>& out)
			{
				std::lock_guard lock(m_mutex);
				smb2dir* dir = m_ctx ? m_api->opendir(m_ctx, p.c_str()) : nullptr;
				if (!dir && (!connect_locked() || !(dir = m_api->opendir(m_ctx, p.c_str()))))
				{
					net_log.error("%s could not be listed", p);
					return false;
				}
				while (smb2dirent* e = m_api->readdir(m_ctx, dir))
				{
					if (!e->name || !strcmp(e->name, ".") || !strcmp(e->name, ".."))
						continue;
					fs::dir_entry d;
					d.name = e->name;
					d.is_directory = e->st.smb2_type == c_type_directory;
					d.size = d.is_directory ? 0 : e->st.smb2_size;
					d.atime = smb_time(e->st.times[0]);
					d.mtime = smb_time(e->st.times[2]);
					d.ctime = smb_time(e->st.times[4]);
					out.push_back(std::move(d));
				}
				m_api->closedir(m_ctx, dir);
				return true;
			}

		private:
			bool connect_locked()
			{
				drop();
				std::string user = m_cred.user, domain;
				if (const usz slash = user.find('\\'); slash != umax) // DOMAIN\user
				{
					domain = user.substr(0, slash);
					user = user.substr(slash + 1);
				}
				m_ctx = m_api->init_context();
				if (!m_ctx)
					return false;
				m_api->set_timeout(m_ctx, 15);
				m_api->set_security_mode(m_ctx, c_signing_enabled);
				if (!domain.empty())
					m_api->set_domain(m_ctx, domain.c_str());
				m_api->set_password(m_ctx, m_cred.password.c_str());
				if (m_api->connect_share(m_ctx, m_cred.server.c_str(), m_cred.share.c_str(), user.c_str()) < 0)
				{
					net_log.error("%s\\%s: %s", m_cred.server, m_cred.share, m_api->get_error(m_ctx));
					m_api->destroy_context(m_ctx);
					m_ctx = nullptr;
					return false;
				}
				m_max_read = std::max<u32>(64 * 1024, std::min<u32>(m_api->get_max_read_size(m_ctx), 1u << 20));
				m_generation++; // every file handle of the old connection is gone
				return true;
			}

			void drop()
			{
				if (m_ctx)
				{
					m_api->disconnect_share(m_ctx);
					m_api->destroy_context(m_ctx);
				}
				m_ctx = nullptr;
			}

			const smb2_api* m_api;
			cred m_cred;
			std::string m_base; // the game's folder (or the folder of the .iso) on the share, '\' separated
			smb2_context* m_ctx = nullptr;
			u32 m_max_read = 64 * 1024;
			u64 m_generation = 0;
			std::mutex m_mutex;
		};

		// One file of the share: its own handle and a few 1 MB cache pieces (an .iso gets more)
		class nas_file final : public fs::file_base
		{
		public:
			static constexpr u64 c_chunk = 1 << 20;

			nas_file(std::shared_ptr<nas_device> dev, std::string path, fs::stat_t st)
				: m_dev(std::move(dev)), m_path(std::move(path)), m_stat(st)
			{
				m_slots.resize(st.size > (512ull << 20) ? 32 : 4);
			}

			~nas_file() override { m_dev->close(m_fh, m_gen); }

			fs::stat_t get_stat() override { return m_stat; }
			bool trunc(u64) override { fs::g_tls_error = fs::error::acces; return false; }
			u64 write(const void*, u64) override { fs::g_tls_error = fs::error::acces; return 0; }
			u64 size() override { return m_stat.size; }

			u64 read(void* buffer, u64 count) override
			{
				const u64 n = read_at(m_pos, buffer, count);
				m_pos += n;
				return n;
			}

			u64 seek(s64 offset, fs::seek_mode whence) override
			{
				const s64 base = whence == fs::seek_set ? 0 : whence == fs::seek_cur ? static_cast<s64>(m_pos) : static_cast<s64>(m_stat.size);
				const s64 to = base + offset;
				if (to < 0)
				{
					fs::g_tls_error = fs::error::inval;
					return -1;
				}
				m_pos = static_cast<u64>(to);
				return m_pos;
			}

			u64 read_at(u64 offset, void* buffer, u64 count) override
			{
				if (offset >= m_stat.size)
					return 0;
				count = std::min<u64>(count, m_stat.size - offset);
				std::lock_guard lock(m_mutex);
				auto* out = static_cast<u8*>(buffer);
				u64 done = 0;
				while (done < count)
				{
					const u64 at = offset + done;
					const u64 chunk = at / c_chunk;
					slot* s = fetch(chunk);
					if (!s)
						break;
					const usz in = static_cast<usz>(at - chunk * c_chunk);
					const usz n = static_cast<usz>(std::min<u64>(count - done, s->data.size() - in));
					if (!n)
						break;
					std::memcpy(out + done, s->data.data() + in, n);
					done += n;
				}
				return done;
			}

		private:
			struct slot
			{
				s64 chunk = -1;
				u64 used = 0;
				std::vector<u8> data;
			};

			slot* fetch(u64 chunk)
			{
				slot* pick = nullptr;
				for (auto& s : m_slots)
				{
					if (s.chunk == static_cast<s64>(chunk))
					{
						s.used = ++m_clock;
						return &s;
					}
					if (!pick || s.used < pick->used)
						pick = &s; // the empty one, or the one read longest ago
				}
				const u64 at = chunk * c_chunk;
				const usz len = static_cast<usz>(std::min<u64>(c_chunk, m_stat.size - at));
				pick->chunk = -1;
				pick->data.resize(len);
				if (!m_dev->read_at(m_path, &m_fh, &m_gen, at, pick->data.data(), len))
				{
					net_log.error("%s could not be read at 0x%x", m_path, at);
					return nullptr;
				}
				pick->chunk = static_cast<s64>(chunk);
				pick->used = ++m_clock;
				return pick;
			}

			std::shared_ptr<nas_device> m_dev;
			std::string m_path;
			fs::stat_t m_stat;
			u64 m_pos = 0;
			smb2fh* m_fh = nullptr;
			u64 m_gen = 0;
			std::mutex m_mutex;
			std::vector<slot> m_slots;
			u64 m_clock = 0;
		};

		class nas_dir final : public fs::dir_base
		{
		public:
			explicit nas_dir(std::vector<fs::dir_entry> entries) : m_entries(std::move(entries)) {}

			bool read(fs::dir_entry& out) override
			{
				if (m_pos >= m_entries.size())
					return false;
				out = m_entries[m_pos++];
				return true;
			}

			void rewind() override { m_pos = 0; }

		private:
			std::vector<fs::dir_entry> m_entries;
			usz m_pos = 0;
		};

		// the device in use; the files it opens keep it alive while RPCS3 holds them
		std::shared_ptr<nas_device> g_device;
		std::mutex g_device_mutex;

		// fs:: holds its devices as stx::shared_ptr: a thin one that forwards to the device of the current game
		class nas_device_ref final : public fs::device_base
		{
		public:
			nas_device_ref() { fs_prefix = std::string(nas_device::c_prefix); }

			static std::shared_ptr<nas_device> get()
			{
				std::lock_guard lock(g_device_mutex);
				return g_device;
			}

			bool stat(const std::string& path, fs::stat_t& info) override
			{
				if (const auto d = get())
					return d->stat(path, info);
				fs::g_tls_error = fs::error::noent;
				return false;
			}
			bool statfs(const std::string& path, fs::device_stat& info) override
			{
				if (const auto d = get())
					return d->statfs(path, info);
				fs::g_tls_error = fs::error::noent;
				return false;
			}
			bool remove_dir(const std::string&) override { fs::g_tls_error = fs::error::acces; return false; }
			bool create_dir(const std::string&) override { fs::g_tls_error = fs::error::acces; return false; }
			bool rename(const std::string&, const std::string&) override { fs::g_tls_error = fs::error::acces; return false; }
			bool remove(const std::string&) override { fs::g_tls_error = fs::error::acces; return false; }
			bool trunc(const std::string&, u64) override { fs::g_tls_error = fs::error::acces; return false; }
			std::unique_ptr<fs::file_base> open(const std::string& path, bs_t<fs::open_mode> mode) override
			{
				if (const auto d = get())
					return d->open(path, mode);
				fs::g_tls_error = fs::error::noent;
				return nullptr;
			}
			std::unique_ptr<fs::dir_base> open_dir(const std::string& path) override
			{
				if (const auto d = get())
					return d->open_dir(path);
				fs::g_tls_error = fs::error::noent;
				return nullptr;
			}
		};

		std::unique_ptr<fs::file_base> nas_device::open(const std::string& path, bs_t<fs::open_mode> mode)
		{
			if (mode & (fs::write + fs::append + fs::create + fs::trunc + fs::excl))
			{
				fs::g_tls_error = fs::error::acces;
				return nullptr;
			}
			const std::string p = full(path);
			fs::stat_t st;
			if (!stat(path, st))
				return nullptr;
			if (st.is_directory)
			{
				fs::g_tls_error = fs::error::isdir;
				return nullptr;
			}
			std::shared_ptr<nas_device> self;
			{
				std::lock_guard lock(g_device_mutex);
				self = g_device;
			}
			if (self.get() != this)
			{
				fs::g_tls_error = fs::error::noent;
				return nullptr;
			}
			return std::make_unique<nas_file>(std::move(self), p, st);
		}

		std::unique_ptr<fs::dir_base> nas_device::open_dir(const std::string& path)
		{
			std::vector<fs::dir_entry> entries;
			if (!list(full(path), entries))
			{
				fs::g_tls_error = fs::error::noent;
				return nullptr;
			}
			return std::make_unique<nas_dir>(std::move(entries));
		}
	}

	bool is_url(const std::string& path)
	{
		return path.size() > 6 && !_strnicmp(path.c_str(), "smb://", 6);
	}

	std::string open(const std::string& url, const std::string& cred_file, std::string* error)
	{
		auto fail = [&](const std::string& why)
		{
			net_log.error("%s: %s", url, why);
			if (error)
				*error = why;
			return std::string();
		};
		// "smb://server/share/a/b" -> server, share, "a\b"
		if (!is_url(url))
			return fail("not a network game");
		const usz a = url.find('/', 6);
		const usz b = a == umax ? a : url.find('/', a + 1);
		if (a == umax || a == 6 || b == umax || b == a + 1 || b + 1 >= url.size())
			return fail("not a network game");
		const std::string server = url.substr(6, a - 6), share = url.substr(a + 1, b - a - 1);
		std::string path = url.substr(b + 1);
		while (!path.empty() && path.back() == '/')
			path.pop_back();
		std::replace(path.begin(), path.end(), '/', '\\');

		cred c = read_cred(cred_file, server, share);
		const smb2_api* api = load_smb2();
		if (!api)
		{
			wipe(c.password);
			return fail("smb2.dll could not be loaded");
		}

		// The device is rooted at the game's folder: for "...\Game\PS3_DISC.SFB" or a folder, that folder;
		// for a .iso, the folder holding it (the .iso is then a file of the device)
		std::string base = path, leaf;
		const usz cut = path.find_last_of('\\');
		const std::string last = cut == umax ? path : path.substr(cut + 1);
		const bool is_iso = last.size() > 4 && !_stricmp(last.c_str() + last.size() - 4, ".iso");
		const bool is_sfb = !_stricmp(last.c_str(), "PS3_DISC.SFB");
		if (is_iso || is_sfb)
		{
			base = cut == umax ? std::string() : path.substr(0, cut);
			leaf = is_iso ? last : std::string();
		}

		auto dev = std::make_shared<nas_device>(api, std::move(c), base);
		const u64 start = GetTickCount64();
		if (!dev->connect())
			return fail(fmt::format("could not connect to %s\\%s", server, share));
		// the game itself must be there: the .iso, or the game folder (a game at the top of the share has no folder
		// of its own, and libsmb2 has no name for the share's top: stat "." failed for every .iso there)
		smb2_stat_64 st{};
		const std::string game = is_iso ? (base.empty() ? leaf : base + "\\" + leaf) : base;
		if (!game.empty() && !dev->stat_full(game, st))
			return fail(fmt::format("%s is not on %s\\%s", game, server, share));

		{
			std::lock_guard lock(g_device_mutex);
			g_device = std::move(dev);
		}
		// registered once; it forwards to the current game's device
		static const bool registered = [](){ fs::set_virtual_device(std::string(nas_device::c_name), stx::make_shared<nas_device_ref>()); return true; }();
		(void)registered;

		const std::string root = std::string(nas_device::c_prefix);
		const std::string result = leaf.empty() ? root : root + "/" + leaf;
		net_log.notice("network game %s\\%s\\%s opened in %u ms -> %s", server, share, path, GetTickCount64() - start, result);
		return result;
	}
}
