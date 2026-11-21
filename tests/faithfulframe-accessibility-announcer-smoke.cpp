#include <QAccessible>
#include <QCoreApplication>
#include <QObject>
#include <cstdio>
#include <vector>
#include "faithfulframe-accessibility-announcer.hpp"

namespace {

struct captured_announcement
{
	QString message;
	QAccessible::AnnouncementPoliteness politeness;
};

std::vector<captured_announcement> announcements;

void capture_accessibility_event(QAccessibleEvent *event)
{
	if (event->type() != QAccessible::Announcement)
		return;
	const auto *announcement = dynamic_cast<const QAccessibleAnnouncementEvent *>(event);
	if (announcement)
		announcements.push_back({announcement->message(), announcement->politeness()});
}

bool expect_announcement(QObject *object, const QString &message, size_t expected_count)
{
	faithfulframe::accessibility::announce_polite(object, message);
	return announcements.size() == expected_count &&
	       (expected_count == 0 || announcements.back().message == message) &&
	       (expected_count == 0 || announcements.back().politeness == QAccessible::AnnouncementPoliteness::Polite);
}

bool expect_no_announcement(QObject *object, const QString &message, size_t expected_count)
{
	faithfulframe::accessibility::announce_polite(object, message);
	return announcements.size() == expected_count;
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	QObject status_object;
	const auto previous_handler = QAccessible::installUpdateHandler(capture_accessibility_event);
	QAccessible::setActive(true);

	faithfulframe::accessibility::status_announcer announcer;
	auto message = announcer.update("searching:awaiting-selection", "Choose a face", "");
	if (message != "Choose a face" || !expect_announcement(&status_object, message, 1)) {
		std::fprintf(stderr, "initial tracking status was not announced politely\n");
		QAccessible::installUpdateHandler(previous_handler);
		return 1;
	}

	// A changed countdown is presentation detail, so the caller keeps the same
	// semantic key and the volunteer does not hear a repeated announcement.
	message = announcer.update("searching:awaiting-selection", "Choose a face in 4.7 seconds", "");
	if (!message.isEmpty() || !expect_no_announcement(&status_object, message, 1)) {
		std::fprintf(stderr, "unchanged tracking state repeated an announcement\n");
		QAccessible::installUpdateHandler(previous_handler);
		return 1;
	}

	message = announcer.update("locked:selected", "Following the selected speaker", "");
	if (message != "Following the selected speaker" || !expect_announcement(&status_object, message, 2)) {
		std::fprintf(stderr, "tracking state change was not announced\n");
		QAccessible::installUpdateHandler(previous_handler);
		return 1;
	}

	message = announcer.update("locked:selected", "Following the selected speaker", "Selection expired");
	if (message != "Selection expired. Following the selected speaker" ||
	    !expect_announcement(&status_object, message, 3)) {
		std::fprintf(stderr, "new operator notice was not announced\n");
		QAccessible::installUpdateHandler(previous_handler);
		return 1;
	}

	message = announcer.update("locked:selected", "Following the selected speaker", "Selection expired");
	if (!message.isEmpty() || !expect_no_announcement(&status_object, message, 3)) {
		std::fprintf(stderr, "repeated operator notice was announced more than once\n");
		QAccessible::installUpdateHandler(previous_handler);
		return 1;
	}

	announcer.reset();
	message = announcer.update("locked:selected", "Following the selected speaker", "");
	if (message != "Following the selected speaker" || !expect_announcement(&status_object, message, 4)) {
		std::fprintf(stderr, "reset status did not announce the current state\n");
		QAccessible::installUpdateHandler(previous_handler);
		return 1;
	}

	QAccessible::installUpdateHandler(previous_handler);
	return 0;
}
