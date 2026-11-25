#pragma once

#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

// Inject monotonic seconds and PSI samples; missing data cannot rearm.
class PressurePause {
	std::optional<double> since, last;
	bool armed = true;

public:
	bool sample(double now, std::optional<double> value)
	{
		if (last && (now < *last || now - *last > 2)) since.reset();
		last = now;
		if (!value || !std::isfinite(*value) || *value < 0 || *value > 100) {
			since.reset();
			return false;
		}
		if (*value <= 10) {
			armed = true;
			since.reset();
		} else if (*value >= 20 && armed) {
			if (!since) since = now;
			if (now - *since >= 10) {
				armed = false;
				since.reset();
				return true;
			}
		} else {
			since.reset();
		}
		return false;
	}
};

inline std::optional<double> read_pressure(const std::string &path, const std::string &category = "some",
	const std::string &average = "avg10")
{
	std::ifstream input(path);
	std::string line;
	while (std::getline(input, line)) {
		std::istringstream fields(line);
		std::string kind, value;
		fields >> kind >> value;
		const auto prefix = average + "=";
		if (kind != category) continue;
		while (fields && value.rfind(prefix, 0) != 0) fields >> value;
		if (value.rfind(prefix, 0) != 0) continue;
		try {
			size_t end;
			double result = std::stod(value.substr(prefix.size()), &end);
			if (end == value.size() - prefix.size() && std::isfinite(result) && result >= 0 && result <= 100)
				return result;
		} catch (...) {}
	}
	return std::nullopt;
}
