// RPCS3 for Game Vault: PS3 shader sharing with cdn.gamevault.fyi.
// When a game's shader cache loads, the console first takes the shaders other players already shared for that game
// (the game's own shader programs raw/<hash>.vp|.fp and its draw setups pipelines/<class>/<version>/*.bin: nothing of
// the player or the console), so a first start compiles them in the "Preloading cached shaders" step like a later
// start does, not in the middle of play. While the game runs, the files this console added are sent back (30 s after
// the start, then every 10 minutes). The server shares a file once two consoles sent it. Every step has a short
// time limit; no network = the game starts as before.

#include "stdafx.h"
#include "uwp_telemetry.h"
#include "uwp_update.h"
#include "Utilities/File.h"
#include "util/logs.hpp"

#include <windows.h>
#include <curl/curl.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if __has_include("uwp_share_key.h")
#include "uwp_share_key.h" // (made by the CI build from the GV_SHARE_KEY secret; never in git)
#endif
#ifndef GV_SHARE_KEY
#define GV_SHARE_KEY ""
#endif

LOG_CHANNEL(gvs_log, "ShaderShare");

namespace uwp_shader_share
{
	namespace
	{
		constexpr const char* c_server = "https://cdn.gamevault.fyi";
		constexpr usz c_max_pack = 24u << 20;
		constexpr usz c_max_send = 8u << 20;   // one upload; the rest goes in the next one
		std::atomic<u32> g_session{0};

		usz curl_append(char* data, usz size, usz n, void* user)
		{
			std::string& out = *static_cast<std::string*>(user);
			if (out.size() + size * n > c_max_pack)
				return 0;
			out.append(data, size * n);
			return size * n;
		}

		// GET when body is null, else POST. Returns the HTTP status (0 = not reached).
		long http(const std::string& url, const std::string* body, long timeout_s, std::string& answer)
		{
			answer.clear();
			CURL* c = curl_easy_init();
			if (!c)
				return 0;
			const std::string agent = fmt::format("RPCS3Xbox/%s", uwp_update::app_version());
			const std::string_view roots = uwp_telemetry::tls_roots();
			curl_blob blob{const_cast<char*>(roots.data()), roots.size(), CURL_BLOB_NOCOPY};
			curl_slist* headers = nullptr;
			const std::string key = std::string("X-GV-Key: ") + GV_SHARE_KEY;
			headers = curl_slist_append(headers, key.c_str());
			headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
			curl_easy_setopt(c, CURLOPT_URL, url.c_str());
			curl_easy_setopt(c, CURLOPT_USERAGENT, agent.c_str());
			curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &blob);
			curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 4L);
			curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
			curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
			curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_append);
			curl_easy_setopt(c, CURLOPT_WRITEDATA, &answer);
			if (body)
			{
				curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
				curl_easy_setopt(c, CURLOPT_POSTFIELDS, body->data());
				curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body->size()));
			}
			long status = 0;
			if (curl_easy_perform(c) == CURLE_OK)
				curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
			curl_easy_cleanup(c);
			curl_slist_free_all(headers);
			return status;
		}

		struct game_set
		{
			std::string root;     // <ppu cache>/shaders_cache/ (no trailing slash kept)
			std::string pipe_dir; // root + /pipelines/<class>/<version>
			std::string game;     // <SERIAL>/<ppu folder>
			std::string set;      // <class>-<version>
		};

		const std::regex c_raw_re(R"(^raw/([0-9A-F]{1,16})\.(vp|fp)$)");
		const std::regex c_pipe_re(R"(^p/[0-9A-F]{1,16}\+[0-9A-F]{1,16}\+[0-9A-F]{1,16}\+[0-9A-F]{1,16}\.bin$)");

		// The file on disk for a pack name, "" for a name that is not one of ours.
		std::string local_path(const game_set& g, const std::string& name)
		{
			if (std::regex_match(name, c_raw_re))
				return g.root + "/" + name;
			if (std::regex_match(name, c_pipe_re))
				return g.pipe_dir + "/" + name.substr(2);
			return {};
		}

		std::set<std::string> read_sent(const game_set& g)
		{
			std::set<std::string> sent;
			const std::string text = fs::file(g.root + "/gvshare_sent.txt").to_string();
			usz start = 0;
			while (start < text.size())
			{
				usz end = text.find('\n', start);
				if (end == umax)
					end = text.size();
				if (end > start)
					sent.insert(text.substr(start, end - start));
				start = end + 1;
			}
			return sent;
		}

		void add_sent(const game_set& g, const std::vector<std::string>& names)
		{
			std::string lines;
			for (const auto& n : names)
				lines += n + "\n";
			if (fs::file f{g.root + "/gvshare_sent.txt", fs::write + fs::create + fs::append})
				f.write(lines);
		}

		void put_entry(std::string& pack, const std::string& name, const std::string& data)
		{
			const u16 nl = static_cast<u16>(name.size());
			const u32 dl = static_cast<u32>(data.size());
			pack.append(reinterpret_cast<const char*>(&nl), 2);
			pack += name;
			pack.append(reinterpret_cast<const char*>(&dl), 4);
			pack += data;
		}

		// The shared files this console has not got yet. Returns how many were written.
		usz take(const game_set& g)
		{
			std::string pack;
			const long status = http(fmt::format("%s/ps3/shaders/%s/%s.pack", c_server, g.game, g.set), nullptr, 8, pack);
			if (status != 200 || pack.size() < 4 || pack.compare(0, 4, "GVP3") != 0)
			{
				gvs_log.notice("No shared shaders for %s (%s)", g.game, status ? fmt::format("HTTP %d", status) : "not reached");
				return 0;
			}
			usz pos = 4, written = 0;
			std::vector<std::string> taken;
			while (pos + 2 <= pack.size())
			{
				u16 nl;
				std::memcpy(&nl, pack.data() + pos, 2);
				pos += 2;
				if (!nl || pos + nl + 4 > pack.size())
					break;
				const std::string name = pack.substr(pos, nl);
				pos += nl;
				u32 dl;
				std::memcpy(&dl, pack.data() + pos, 4);
				pos += 4;
				if (pos + dl > pack.size())
					break;
				const std::string path = local_path(g, name);
				if (!path.empty() && !fs::is_file(path) && fs::write_file(path, fs::create + fs::excl, pack.data() + pos, dl))
				{
					written++;
					taken.push_back(name);
				}
				pos += dl;
			}
			add_sent(g, taken); // (taken from the server, so never sent back)
			return written;
		}

		// Sends the files this console made that the server has not had from it. Returns how many were sent.
		usz send(const game_set& g)
		{
			if (!*GV_SHARE_KEY)
				return 0;
			const std::set<std::string> sent = read_sent(g);
			std::string pack = "GVP3";
			std::vector<std::string> names;
			auto add_dir = [&](const std::string& dir, const std::string& prefix, bool pipes)
			{
				for (const auto& e : fs::dir(dir))
				{
					if (e.is_directory || !e.size || pack.size() > c_max_send)
						continue;
					const std::string name = prefix + e.name;
					if (sent.count(name) || !std::regex_match(name, pipes ? c_pipe_re : c_raw_re))
						continue;
					const std::string data = fs::file(dir + "/" + e.name).to_string();
					if (data.empty())
						continue;
					put_entry(pack, name, data);
					names.push_back(name);
				}
			};
			add_dir(g.root + "/raw", "raw/", false);
			add_dir(g.pipe_dir, "p/", true);
			if (names.empty())
				return 0;
			std::string answer;
			const long status = http(fmt::format("%s/api/ps3/v1/upload?game=%s&set=%s", c_server, g.game, g.set), &pack, 60, answer);
			if (status != 200)
			{
				gvs_log.warning("Sending %u shader files did not go through (%d), tried again later", names.size(), status);
				return 0;
			}
			add_sent(g, names);
			gvs_log.notice("Sent %u shader files (%u KB): %s", names.size(), pack.size() >> 10, answer);
			return names.size();
		}
	}

	// rsx_cache.h shaders_cache::load(), before the cache folder is read.
	void on_cache_load(const std::string& root_path, const std::string& pipeline_class, const std::string& version)
	{
		const u32 session = ++g_session;
		game_set g;
		g.root = root_path;
		while (!g.root.empty() && (g.root.back() == '/' || g.root.back() == '\\'))
			g.root.pop_back();
		// <...>/<SERIAL>/<ppu folder>/shaders_cache
		const usz a = g.root.find_last_of("/\\");
		const usz b = a == umax || !a ? umax : g.root.find_last_of("/\\", a - 1);
		const usz c = b == umax || !b ? umax : g.root.find_last_of("/\\", b - 1);
		if (c == umax || g.root.substr(a + 1) != "shaders_cache")
			return;
		g.game = g.root.substr(c + 1, b - c - 1) + "/" + g.root.substr(b + 1, a - b - 1);
		std::string cls = pipeline_class;
		for (char& ch : cls)
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		g.set = cls + "-" + version;
		if (!std::regex_match(g.game, std::regex(R"(^[A-Z]{4}[0-9]{5}/[A-Za-z0-9._+\-]{1,120}$)")) ||
			!std::regex_match(g.set, std::regex(R"(^[a-z0-9_]{1,24}-v[0-9]{1,3}$)")))
		{
			gvs_log.notice("Shader sharing: %s / %s is not a game we share for", g.game, g.set);
			return;
		}
		g.pipe_dir = g.root + "/pipelines/" + pipeline_class + "/" + version;
		fs::create_path(g.pipe_dir);
		fs::create_path(g.root + "/raw");

		const auto t0 = std::chrono::steady_clock::now();
		const usz got = take(g);
		gvs_log.notice("Shader sharing: %s took %u shader files from other players (%u ms)", g.game, got,
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());

		// In the background: what this console made last time (30 s in), then every 10 minutes while this game runs.
		std::thread([g, session]()
		{
			auto wait = std::chrono::seconds(30);
			for (;;)
			{
				const auto until = std::chrono::steady_clock::now() + wait;
				while (std::chrono::steady_clock::now() < until)
				{
					if (g_session.load() != session)
						return;
					std::this_thread::sleep_for(std::chrono::seconds(5));
				}
				if (g_session.load() != session)
					return;
				send(g);
				wait = std::chrono::minutes(10);
			}
		}).detach();
	}
}
