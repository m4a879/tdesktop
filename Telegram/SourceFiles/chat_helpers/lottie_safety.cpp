/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/lottie_safety.h"

#include "lottie/lottie_common.h"
#include "ui/image/image_prepare.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace LottieSafety {
namespace {

constexpr auto kMaxCanvasWidth = 32000;
constexpr auto kMaxCanvasHeight = 19000;
constexpr auto kMaxEmbeddedSide = 4096;
constexpr auto kMaxEmbeddedBytes = int64(500) * 1024 * 1024;
constexpr auto kMaxLayers = 2048;
constexpr auto kMaxAssets = 4096;
constexpr auto kMaxPrecompDepth = 32;

bool IsGzip(const QByteArray &bytes) {
	return (bytes.size() >= 2)
		&& (uint8(bytes[0]) == 0x1F)
		&& (uint8(bytes[1]) == 0x8B);
}

bool LooksLikeJson(const QByteArray &bytes) {
	auto index = 0;
	const auto size = bytes.size();
	if (size >= 3
		&& uint8(bytes[0]) == 0xEF
		&& uint8(bytes[1]) == 0xBB
		&& uint8(bytes[2]) == 0xBF) {
		index = 3;
	}
	while (index < size) {
		const auto c = bytes[index];
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
			break;
		}
		++index;
	}
	return (index < size) && (bytes[index] == '{');
}

struct BitmapDimensions {
	int width = 0;
	int height = 0;
};

std::optional<BitmapDimensions> PngDimensions(const QByteArray &data) {
	if (!data.startsWith("\x89PNG\r\n\x1a\n") || data.size() < 24) {
		return std::nullopt;
	}
	const auto read = [&](int offset) {
		return (uint32(uint8(data[offset])) << 24)
			| (uint32(uint8(data[offset + 1])) << 16)
			| (uint32(uint8(data[offset + 2])) << 8)
			| uint32(uint8(data[offset + 3]));
	};
	return BitmapDimensions{ int(read(16)), int(read(20)) };
}

std::optional<BitmapDimensions> JpegDimensions(const QByteArray &data) {
	const auto size = data.size();
	if (size < 4 || uint8(data[0]) != 0xFF || uint8(data[1]) != 0xD8) {
		return std::nullopt;
	}
	auto i = 2;
	while (i + 4 <= size) {
		if (uint8(data[i]) != 0xFF) {
			++i;
			continue;
		}
		const auto marker = uint8(data[i + 1]);
		if (marker == 0x00 || marker == 0x01 || marker == 0xFF
			|| (marker >= 0xD0 && marker <= 0xD7)) {
			i += 2;
			continue;
		} else if (marker == 0xD9 || marker == 0xDA) {
			return std::nullopt;
		}
		const auto length = (uint32(uint8(data[i + 2])) << 8)
			| uint32(uint8(data[i + 3]));
		if (length < 2) {
			return std::nullopt;
		}
		if (marker >= 0xC0
			&& marker <= 0xCF
			&& marker != 0xC4
			&& marker != 0xC8
			&& marker != 0xCC) {
			if (i + 9 > size) {
				return std::nullopt;
			}
			return BitmapDimensions{
				int((uint32(uint8(data[i + 7])) << 8)
					| uint32(uint8(data[i + 8]))),
				int((uint32(uint8(data[i + 5])) << 8)
					| uint32(uint8(data[i + 6]))),
			};
		}
		i += 2 + int(length);
	}
	return std::nullopt;
}

bool WithinLimits(const BitmapDimensions &dimensions) {
	return (dimensions.width >= 1)
		&& (dimensions.height >= 1)
		&& (dimensions.width <= kMaxEmbeddedSide)
		&& (dimensions.height <= kMaxEmbeddedSide);
}

[[nodiscard]] bool CountLayers(
		const QJsonValue &layers,
		int &total,
		int depth) {
	if (!layers.isArray()) {
		return true;
	} else if (depth > kMaxPrecompDepth) {
		return false;
	}
	for (const auto &item : layers.toArray()) {
		if (!item.isObject()) {
			continue;
		}
		if (++total > kMaxLayers) {
			return false;
		}
		if (!CountLayers(item.toObject().value("layers"), total, depth + 1)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool CheckEmbeddedImage(
		const QJsonObject &asset,
		int64 &totalBytes) {
	const auto width = asset.value("w").toDouble();
	const auto height = asset.value("h").toDouble();
	if (width < 1.
		|| height < 1.
		|| width > kMaxEmbeddedSide
		|| height > kMaxEmbeddedSide) {
		return false;
	}
	totalBytes += int64(width) * int64(height) * 4;
	if (totalBytes > kMaxEmbeddedBytes) {
		return false;
	}
	const auto raw = QByteArray::fromBase64(
		asset.value("p").toString().toLatin1());
	if (const auto dims = PngDimensions(raw)) {
		return WithinLimits(*dims);
	} else if (const auto dims = JpegDimensions(raw)) {
		return WithinLimits(*dims);
	}
	return true;
}

[[nodiscard]] bool CheckAssets(const QJsonValue &assets, int64 &totalBytes) {
	if (!assets.isArray()) {
		return true;
	}
	auto layers = 0;
	auto count = 0;
	for (const auto &item : assets.toArray()) {
		if (++count > kMaxAssets) {
			return false;
		}
		const auto asset = item.toObject();
		if (!CountLayers(asset.value("layers"), layers, 0)) {
			return false;
		}
		const auto image = asset.value("p");
		if (image.isString() && !image.toString().isEmpty()
			&& !CheckEmbeddedImage(asset, totalBytes)) {
			return false;
		}
	}
	return true;
}

}

bool ValidJson(const QByteArray &json) {
	if (json.isEmpty()) {
		return true;
	} else if (json.size() > Lottie::kMaxFileSize) {
		return false;
	}
	const auto document = QJsonDocument::fromJson(json);
	const auto root = document.object();
	if (document.isNull() || root.isEmpty()) {
		return false;
	}
	const auto width = root.value("w").toDouble();
	const auto height = root.value("h").toDouble();
	if (width < 1.
		|| height < 1.
		|| width > kMaxCanvasWidth
		|| height > kMaxCanvasHeight) {
		return false;
	}
	auto layers = 0;
	if (!CountLayers(root.value("layers"), layers, 0)) {
		return false;
	}
	auto embeddedBytes = int64(0);
	return CheckAssets(root.value("assets"), embeddedBytes);
}

ContentCheck CheckContent(const QByteArray &bytes) {
	if (IsGzip(bytes)) {
		return { true, ValidJson(Images::UnpackGzip(bytes)) };
	} else if (LooksLikeJson(bytes)) {
		return { true, ValidJson(bytes) };
	}
	return { false, false };
}

QByteArray CheckedContent(const QByteArray &data, const QString &filepath) {
	auto content = Lottie::ReadContent(data, filepath);
	if (!content.isEmpty()) {
		const auto check = CheckContent(content);
		if (check.lottie && !check.valid) {
			return QByteArray();
		}
	}
	return content;
}

}
