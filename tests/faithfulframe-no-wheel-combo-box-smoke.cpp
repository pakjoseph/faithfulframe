#include "faithfulframe-no-wheel-policy.hpp"

#include <QEvent>
#include <cstdio>

static_assert(faithfulframe::ui::selector_focus_policy == Qt::StrongFocus);

int main()
{
	QEvent event(QEvent::Wheel);
	if (!event.isAccepted()) {
		std::fprintf(stderr, "wheel events must begin accepted before the selector rejects them\n");
		return 2;
	}

	faithfulframe::ui::ignore_selector_wheel_event(&event);
	if (event.isAccepted()) {
		std::fprintf(stderr, "tracking selector wheel policy did not ignore the event\n");
		return 1;
	}

	return 0;
}
