#include "faithfulframe-candidate-selector.hpp"

#include <QApplication>
#include <QAbstractItemView>
#include <QKeyEvent>

#include <cstdio>
#include <cstdlib>

static void check_impl(bool condition, int line)
{
	if (!condition) {
		std::fprintf(stderr, "candidate selector check failed at line %d\n", line);
		std::abort();
	}
}

#define check(condition) check_impl((condition), __LINE__)

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QComboBox selector;
	selector.addItems({"Choose a detected face", "Visible face 1", "Visible face 2"});
	selector.setCurrentIndex(1);
	check(faithfulframe::ui::candidate_selector_model_can_refresh(&selector));
	selector.show();
	selector.showPopup();
	app.processEvents();
	check(selector.view()->isVisible());
	check(!faithfulframe::ui::candidate_selector_model_can_refresh(&selector));
	const QModelIndex initial_row = selector.view()->currentIndex();
	QKeyEvent move_highlight(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
	QApplication::sendEvent(selector.view(), &move_highlight);
	app.processEvents();
	const QModelIndex highlighted_row = selector.view()->currentIndex();
	check(highlighted_row.isValid() && highlighted_row != initial_row);

	// Model refreshes are deferred while the popup is open, preserving the
	// volunteer's keyboard-highlighted candidate row.
	if (faithfulframe::ui::candidate_selector_model_can_refresh(&selector)) {
		selector.clear();
		selector.addItems({"Choose a detected face", "Refreshed face 1"});
	}
	check(selector.view()->currentIndex() == highlighted_row);

	selector.hidePopup();
	app.processEvents();
	check(faithfulframe::ui::candidate_selector_model_can_refresh(&selector));
	selector.clear();
	selector.addItems({"Choose a detected face", "Refreshed face 1"});
	check(selector.currentIndex() == 0);
	selector.hide();
	return 0;
}
