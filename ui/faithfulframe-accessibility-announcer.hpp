#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QAccessibleAnnouncementEvent>
#endif

namespace faithfulframe::accessibility {

// Announce only semantic status changes and new operator notices. Callers can
// refresh detailed status text more frequently without repeating speech.
class status_announcer {
	QString last_status_key;
	QString last_operator_notice;

public:
	QString update(const QString &status_key, const QString &status_message, const QString &operator_notice)
	{
		const bool status_changed = status_key != last_status_key;
		const bool notice_changed = !operator_notice.isEmpty() && operator_notice != last_operator_notice;
		if (!status_changed && !notice_changed) {
			if (operator_notice.isEmpty())
				last_operator_notice.clear();
			return {};
		}

		last_status_key = status_key;
		if (notice_changed)
			last_operator_notice = operator_notice;
		else if (operator_notice.isEmpty())
			last_operator_notice.clear();

		return notice_changed ? operator_notice + ". " + status_message : status_message;
	}

	void reset()
	{
		last_status_key.clear();
		last_operator_notice.clear();
	}
};

inline void announce_polite(QObject *object, const QString &message)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	if (!object || message.trimmed().isEmpty())
		return;
	QAccessibleAnnouncementEvent event(object, message);
	event.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
	QAccessible::updateAccessibility(&event);
#else
	(void)object;
	(void)message;
#endif
}

} // namespace faithfulframe::accessibility
