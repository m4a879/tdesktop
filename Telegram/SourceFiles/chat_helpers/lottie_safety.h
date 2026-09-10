/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class QByteArray;
class QString;

namespace LottieSafety {

struct ContentCheck {
	bool lottie = false;
	bool valid = false;
};

[[nodiscard]] ContentCheck CheckContent(const QByteArray &bytes);

[[nodiscard]] QByteArray CheckedContent(
	const QByteArray &data,
	const QString &filepath);

[[nodiscard]] bool ValidJson(const QByteArray &json);

}
