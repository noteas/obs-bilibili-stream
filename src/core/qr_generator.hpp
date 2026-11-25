#pragma once
#include <string>

namespace Core {
class QrGenerator {
public:
	// Encodes `data` as a QR code and writes it as an SVG file to `path`.
	// Returns false when the payload cannot be encoded or the file cannot be
	// written. The file is meant to be handed to the system browser (ADR-0003:
	// the console panel cannot render images).
	static bool writeSvgFile(const std::string &path, const std::string &data);
};
} // namespace Core
