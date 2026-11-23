#ifndef KYTY_COMMON_KNOB_FILE_H_
#define KYTY_COMMON_KNOB_FILE_H_

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

// Values to try on a game without a new build, read from ~/.config/KytyPS5-Native/vrr (a "key=value" line each,
// decimal or 0x hex) at most once a second. 0 when the key is not there.
inline uint64_t KnobFile(const char* key) {
	static std::mutex                                      mutex;
	static std::chrono::steady_clock::time_point           read_at {};
	static std::array<std::pair<std::string, uint64_t>, 8> values {};
	static size_t                                          count = 0;

	std::scoped_lock lock(mutex);
	const auto       now = std::chrono::steady_clock::now();
	if (read_at == std::chrono::steady_clock::time_point {} || now - read_at > std::chrono::seconds(1)) {
		read_at          = now;
		count            = 0;
		const char* base = std::getenv("XDG_CONFIG_HOME");
		const char* home = std::getenv("HOME");
		std::string path = base != nullptr ? std::string(base) : (home != nullptr ? std::string(home) + "/.config" : "");
		std::ifstream file(path + "/KytyPS5-Native/vrr");
		std::string   line;
		while (std::getline(file, line) && count < values.size()) {
			const auto eq = line.find('=');
			if (eq != std::string::npos) {
				values[count++] = {line.substr(0, eq), std::strtoull(line.c_str() + eq + 1, nullptr, 0)};
			}
		}
	}
	for (size_t i = 0; i < count; i++) {
		if (values[i].first == key) {
			return values[i].second;
		}
	}
	return 0;
}

#endif /* KYTY_COMMON_KNOB_FILE_H_ */
