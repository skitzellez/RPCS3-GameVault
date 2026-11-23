#include "stdafx.h"
#include "uwp_update.h"
#include "uwp_telemetry.h"
#include "util/logs.hpp"

#include <windows.h>
#include <curl/curl.h>

#include <mutex>

LOG_CHANNEL(upd_log, "Update");

namespace uwp_update
{
	namespace
	{
		// "latest" skips pre-releases, which is what the port publishes, so this lists them all.
		constexpr const char* c_url = "https://api.github.com/repos/emelin2004/rpcs3-xbox/releases?per_page=10";
		constexpr usz c_max_answer = 2u << 20;

		const std::string g_app_version = RPCS3_XBOX_VERSION;
		std::mutex g_mutex;
		atomic_t<status> g_status = status::unknown;
		std::string g_latest;

		struct start_args
		{
			std::function<void()> on_done;
		};

		usz curl_append(char* data, usz size, usz n, void* user)
		{
			std::string& out = *static_cast<std::string*>(user);
			if (out.size() + size * n > c_max_answer)
				return 0;
			out.append(data, size * n);
			return size * n;
		}

		// A tag like "v0.1.34.75": the version without the "v", or empty for other tags.
		std::string version_of(std::string_view tag)
		{
			if (tag.size() < 2 || (tag[0] != 'v' && tag[0] != 'V'))
				return {};
			tag.remove_prefix(1);
			for (const char c : tag)
				if ((c < '0' || c > '9') && c != '.')
					return {};
			return std::string(tag);
		}

		// The highest vX.Y.Z.W "tag_name" in GitHub's release list.
		std::string newest_tag(const std::string& json)
		{
			std::string best;
			constexpr std::string_view key = "\"tag_name\"";
			for (usz pos = json.find(key); pos != std::string::npos; pos = json.find(key, pos + 1))
			{
				const usz open = json.find('"', json.find(':', pos + key.size()) + 1);
				const usz close = open == std::string::npos ? std::string::npos : json.find('"', open + 1);
				if (close == std::string::npos)
					break;
				const std::string v = version_of(std::string_view(json).substr(open + 1, close - open - 1));
				if (!v.empty() && (best.empty() || compare(v, best) > 0))
					best = v;
			}
			return best;
		}

		std::string fetch(long& http)
		{
			std::string answer;
			CURL* c = curl_easy_init();
			if (!c)
				return answer;
			const std::string agent = fmt::format("RPCS3Xbox/%s", g_app_version);
			curl_slist* list = nullptr;
			list = curl_slist_append(list, "Accept: application/vnd.github+json");
			list = curl_slist_append(list, "X-GitHub-Api-Version: 2022-11-28");
			const std::string_view roots = uwp_telemetry::tls_roots();
			curl_blob blob{const_cast<char*>(roots.data()), roots.size(), CURL_BLOB_NOCOPY};
			char error[CURL_ERROR_SIZE]{};
			curl_easy_setopt(c, CURLOPT_URL, c_url);
			curl_easy_setopt(c, CURLOPT_USERAGENT, agent.c_str());
			curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
			curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &blob);
			curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
			curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
			curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
			curl_easy_setopt(c, CURLOPT_ERRORBUFFER, error);
			curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_append);
			curl_easy_setopt(c, CURLOPT_WRITEDATA, &answer);
			const CURLcode res = curl_easy_perform(c);
			http = 0;
			if (res == CURLE_OK)
				curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
			else
				upd_log.warning("Update check failed: %s (%s)", curl_easy_strerror(res), error);
			curl_slist_free_all(list);
			curl_easy_cleanup(c);
			return answer;
		}

		DWORD WINAPI check_thread(void* arg)
		{
			const std::unique_ptr<start_args> args(static_cast<start_args*>(arg));
			long http = 0;
			const std::string answer = fetch(http);
			if (http != 200)
			{
				if (http)
					upd_log.warning("Update check: GitHub answered %d", http);
				return 0;
			}

			const std::string latest = newest_tag(answer);
			if (latest.empty())
			{
				upd_log.notice("Update check: no releases found");
				return 0;
			}
			const bool outdated = compare(latest, g_app_version) > 0;
			upd_log.notice("Update check: installed %s, newest release %s%s", g_app_version, latest, outdated ? " (update available)" : "");
			{
				std::lock_guard lock(g_mutex);
				g_latest = latest;
			}
			g_status = outdated ? status::outdated : status::current;
			if (args->on_done)
				args->on_done();
			return 0;
		}
	}

	const std::string& app_version()
	{
		return g_app_version;
	}

	void start(std::function<void()> on_done)
	{
		auto* args = new start_args{std::move(on_done)};
		// TLS needs more than the default stack (as the telemetry upload thread).
		if (const HANDLE h = CreateThread(nullptr, 2 * 1024 * 1024, check_thread, args, 0, nullptr))
			CloseHandle(h);
		else
			delete args;
	}

	status get_status()
	{
		return g_status;
	}

	std::string latest_version()
	{
		std::lock_guard lock(g_mutex);
		return g_latest;
	}

	int compare(const std::string& a, const std::string& b)
	{
		usz i = 0, j = 0;
		while (i < a.size() || j < b.size())
		{
			u64 x = 0, y = 0;
			for (; i < a.size() && a[i] != '.'; i++)
				x = x * 10 + (a[i] - '0');
			for (; j < b.size() && b[j] != '.'; j++)
				y = y * 10 + (b[j] - '0');
			if (x != y)
				return x < y ? -1 : 1;
			i++;
			j++;
		}
		return 0;
	}
}
