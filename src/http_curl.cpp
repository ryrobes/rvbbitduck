#include "engine.hpp"

#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#if !defined(__EMSCRIPTEN__)
#include <curl/curl.h>
#endif

namespace duckdb {
namespace rvbbit {

namespace {

#if !defined(__EMSCRIPTEN__)

struct CurlGlobal {
	CurlGlobal() {
		auto code = curl_global_init(CURL_GLOBAL_DEFAULT);
		if (code != 0) {
			ready = false;
		}
	}
	~CurlGlobal() {
		if (ready) {
			curl_global_cleanup();
		}
	}
	bool ready = true;
};

CurlGlobal &GlobalCurl() {
	static CurlGlobal global;
	return global;
}

std::recursive_mutex g_share_mu;

void ShareLock(CURL *, curl_lock_data, curl_lock_access, void *) {
	g_share_mu.lock();
}

void ShareUnlock(CURL *, curl_lock_data, void *) {
	g_share_mu.unlock();
}

struct ShareHandle {
	ShareHandle() {
		share = curl_share_init();
		if (!share) {
			return;
		}
		curl_share_setopt(share, CURLSHOPT_LOCKFUNC, ShareLock);
		curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, ShareUnlock);
		curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
		curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
		curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
	}
	~ShareHandle() {
		if (share) {
			curl_share_cleanup(share);
		}
	}
	CURLSH *share = nullptr;
};

ShareHandle &Shared() {
	static ShareHandle handle;
	return handle;
}

struct Slot {
	CURL *easy = nullptr;
	curl_slist *headers = nullptr;
	string response;
	string retry_after;
	string request_body;
	string url;
	char errbuf[CURL_ERROR_SIZE] {};
	std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	int attempt = 1;
	idx_t index = 0;
};

size_t WriteBody(char *ptr, size_t size, size_t nmemb, void *userdata) {
	auto *slot = static_cast<Slot *>(userdata);
	slot->response.append(ptr, size * nmemb);
	return size * nmemb;
}

size_t WriteHeader(char *ptr, size_t size, size_t nmemb, void *userdata) {
	auto *slot = static_cast<Slot *>(userdata);
	string line(ptr, size * nmemb);
	string lower = line;
	for (auto &ch : lower) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	const string prefix = "retry-after:";
	if (lower.rfind(prefix, 0) == 0) {
		auto value = line.substr(prefix.size());
		auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
		value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
		value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
		slot->retry_after = std::move(value);
	}
	return size * nmemb;
}

int RetryDelayMs(const FetchResult &result, int32_t base_ms, int attempt) {
	auto marker = result.error.find("retry-after=");
	if (marker != string::npos) {
		try {
			int seconds = std::stoi(result.error.substr(marker + 12));
			if (seconds > 0) {
				return std::min(seconds, 5) * 1000;
			}
		} catch (const std::exception &) {
		}
	}
	int delay = std::max(base_ms, 0);
	for (int i = 1; i < attempt; i++) {
		delay = std::min(delay * 2, 2000);
	}
	return delay;
}

bool RetryableStatus(int status) {
	return status == 429 || status == 408 || (status >= 500 && status <= 599) || status == 0;
}

void DestroySlot(CURLM *multi, Slot &slot) {
	if (slot.easy) {
		curl_multi_remove_handle(multi, slot.easy);
		curl_easy_cleanup(slot.easy);
		slot.easy = nullptr;
	}
	if (slot.headers) {
		curl_slist_free_all(slot.headers);
		slot.headers = nullptr;
	}
}

FetchResult FinishSlot(Slot &slot, CURLcode code) {
	FetchResult result;
	long status = 0;
	if (slot.easy) {
		curl_easy_getinfo(slot.easy, CURLINFO_RESPONSE_CODE, &status);
	}
	result.status = static_cast<int>(status);
	result.body = slot.response;
	if (code != CURLE_OK || result.status == 0) {
		result.status = 0;
		string detail = slot.errbuf[0] ? slot.errbuf : curl_easy_strerror(code);
		if (detail.empty()) {
			detail = "request failed before a response";
		}
		result.error = "rvbbit could not connect to " + slot.url + ": " + detail;
		result.ok = false;
		return result;
	}
	result.ok = result.status >= 200 && result.status < 300;
	if (!result.ok) {
		result.error = "HTTP status " + std::to_string(result.status);
		if (!slot.retry_after.empty()) {
			result.error += " retry-after=" + slot.retry_after;
		}
	}
	return result;
}

#endif

} // namespace

void RecordExchange(ClientContext &context, const string &kind, const string &url, int status,
                    const string &request_body, const string &response_body, int64_t latency_ms);

vector<FetchResult> LivePostMany(ClientContext &context, const string &kind, const string &url,
                                 const vector<string> &bodies, const string &api_key, const LivePostOptions &options) {
#if defined(__EMSCRIPTEN__)
	// Hook for a future WASM transport. Native builds never take this branch.
	(void)context;
	(void)kind;
	(void)url;
	(void)bodies;
	(void)api_key;
	(void)options;
	throw InvalidInputException(
	    "rvbbit WASM HTTP transport is not linked yet. Native builds POST with libcurl and do not require httpfs.");
#else
	if (!GlobalCurl().ready) {
		throw InvalidInputException("rvbbit could not initialize libcurl");
	}
	if (!Shared().share) {
		throw InvalidInputException("rvbbit could not initialize the libcurl connection cache");
	}
	vector<FetchResult> results(bodies.size());
	struct Pending {
		idx_t index = 0;
		string body;
		int attempt = 1;
	};
	vector<Pending> pending;
	pending.reserve(bodies.size());
	for (idx_t i = 0; i < bodies.size(); i++) {
		pending.push_back(Pending {i, bodies[i], 1});
	}
	int concurrency = std::max(1, options.concurrency);
	int timeout = options.timeout_sec > 0 ? options.timeout_sec : 60;
	int max_attempts = std::max(0, options.max_retries) + 1;

	while (!pending.empty()) {
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		CURLM *multi = curl_multi_init();
		if (!multi) {
			throw InvalidInputException("rvbbit could not start an HTTP transfer");
		}
		curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
		curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, static_cast<long>(concurrency));
		curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, static_cast<long>(concurrency));

		vector<unique_ptr<Slot>> slots;
		vector<Pending> next_wave;
		string wave_error;
		int delay_ms = 0;
		size_t next = 0;
		int inflight = 0;

		auto enqueue = [&](const Pending &job) {
			auto slot = make_uniq<Slot>();
			slot->request_body = job.body;
			slot->url = url;
			slot->attempt = job.attempt;
			slot->index = job.index;
			slot->easy = curl_easy_init();
			if (!slot->easy) {
				throw InvalidInputException("rvbbit could not start an HTTP transfer");
			}
			slot->headers = curl_slist_append(slot->headers, "Content-Type: application/json");
			slot->headers = curl_slist_append(slot->headers, ("Authorization: Bearer " + api_key).c_str());
			slot->headers = curl_slist_append(slot->headers, "HTTP-Referer: https://github.com/ryrobes/rvbbitduck");
			slot->headers = curl_slist_append(slot->headers, "X-Title: rvbbitduck");
			curl_easy_setopt(slot->easy, CURLOPT_URL, slot->url.c_str());
			curl_easy_setopt(slot->easy, CURLOPT_POST, 1L);
			curl_easy_setopt(slot->easy, CURLOPT_POSTFIELDS, slot->request_body.data());
			curl_easy_setopt(slot->easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(slot->request_body.size()));
			curl_easy_setopt(slot->easy, CURLOPT_HTTPHEADER, slot->headers);
			curl_easy_setopt(slot->easy, CURLOPT_WRITEFUNCTION, WriteBody);
			curl_easy_setopt(slot->easy, CURLOPT_WRITEDATA, slot.get());
			curl_easy_setopt(slot->easy, CURLOPT_HEADERFUNCTION, WriteHeader);
			curl_easy_setopt(slot->easy, CURLOPT_HEADERDATA, slot.get());
			curl_easy_setopt(slot->easy, CURLOPT_ERRORBUFFER, slot->errbuf);
			curl_easy_setopt(slot->easy, CURLOPT_TIMEOUT, static_cast<long>(timeout));
			curl_easy_setopt(slot->easy, CURLOPT_CONNECTTIMEOUT, 10L);
			curl_easy_setopt(slot->easy, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(slot->easy, CURLOPT_TCP_KEEPALIVE, 1L);
			curl_easy_setopt(slot->easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
			curl_easy_setopt(slot->easy, CURLOPT_SHARE, Shared().share);
			curl_easy_setopt(slot->easy, CURLOPT_SSL_VERIFYPEER, options.tls_verify ? 1L : 0L);
			curl_easy_setopt(slot->easy, CURLOPT_SSL_VERIFYHOST, options.tls_verify ? 2L : 0L);
			curl_easy_setopt(slot->easy, CURLOPT_FOLLOWLOCATION, 0L);
			curl_easy_setopt(slot->easy, CURLOPT_USERAGENT, "rvbbitduck");
			slot->started = std::chrono::steady_clock::now();
			curl_multi_add_handle(multi, slot->easy);
			slots.push_back(std::move(slot));
			inflight++;
		};

		while (next < pending.size() && inflight < concurrency) {
			enqueue(pending[next++]);
		}
		while (inflight > 0) {
			if (context.IsInterrupted()) {
				for (auto &slot : slots) {
					DestroySlot(multi, *slot);
				}
				curl_multi_cleanup(multi);
				throw InterruptException();
			}
			int running = 0;
			auto code = curl_multi_perform(multi, &running);
			if (code != CURLM_OK && code != CURLM_CALL_MULTI_PERFORM) {
				for (auto &slot : slots) {
					DestroySlot(multi, *slot);
				}
				curl_multi_cleanup(multi);
				throw InvalidInputException(string("rvbbit HTTP transfer failed: ") + curl_multi_strerror(code));
			}
			int messages = 0;
			while (auto *message = curl_multi_info_read(multi, &messages)) {
				if (message->msg != CURLMSG_DONE) {
					continue;
				}
				Slot *slot = nullptr;
				for (auto &candidate : slots) {
					if (candidate && candidate->easy == message->easy_handle) {
						slot = candidate.get();
						break;
					}
				}
				if (!slot) {
					continue;
				}
				auto result = FinishSlot(*slot, message->data.result);
				auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				                   std::chrono::steady_clock::now() - slot->started)
				                   .count();
				RecordExchange(context, kind, slot->url, result.status, slot->request_body, slot->response,
				               static_cast<int64_t>(elapsed));
				inflight--;
				bool again = !result.ok && RetryableStatus(result.status) && slot->attempt < max_attempts;
				if (again) {
					delay_ms = std::max(delay_ms, RetryDelayMs(result, options.retry_base_ms, slot->attempt));
					next_wave.push_back(Pending {slot->index, slot->request_body, slot->attempt + 1});
				} else {
					results[slot->index] = result;
					if (result.status == 0 && wave_error.empty()) {
						wave_error = result.error;
					}
				}
				DestroySlot(multi, *slot);
			}
			while (next < pending.size() && inflight < concurrency) {
				enqueue(pending[next++]);
			}
			if (inflight > 0) {
				curl_multi_poll(multi, nullptr, 0, 200, nullptr);
			}
		}
		for (auto &slot : slots) {
			if (slot) {
				DestroySlot(multi, *slot);
			}
		}
		curl_multi_cleanup(multi);
		if (!wave_error.empty() && next_wave.empty()) {
			throw InvalidInputException(wave_error);
		}
		if (!next_wave.empty() && delay_ms > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
		}
		pending = std::move(next_wave);
	}
	return results;
#endif
}

} // namespace rvbbit
} // namespace duckdb
