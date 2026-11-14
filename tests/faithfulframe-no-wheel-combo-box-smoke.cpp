#include "faithfulframe-no-wheel-combo-box.hpp"

#include <QApplication>
#include <QProxyStyle>

class WheelEnabledStyle final : public QProxyStyle {
public:
	int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr,
		      QStyleHintReturn *returnData = nullptr) const override
	{
		if (hint == QStyle::SH_ComboBox_AllowWheelScrolling)
			return 1;
		return QProxyStyle::styleHint(hint, option, widget, returnData);
	}
};

static bool send_wheel(FaithfulFrameNoWheelComboBox &combo)
{
	QWheelEvent event(QPointF(4.0, 4.0), QPointF(4.0, 4.0), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
			  Qt::NoScrollPhase, false);
	QApplication::sendEvent(&combo, &event);
	return !event.isAccepted() && combo.currentIndex() == 0;
}

int main(int argc, char **argv)
{
	QApplication application(argc, argv);
	WheelEnabledStyle style;
	FaithfulFrameNoWheelComboBox combo;
	combo.setStyle(&style);
	combo.addItems({"Camera one", "Camera two"});
	combo.setCurrentIndex(0);
	combo.show();
	application.processEvents();

	if (combo.focusPolicy() != Qt::StrongFocus ||
	    style.styleHint(QStyle::SH_ComboBox_AllowWheelScrolling, nullptr, &combo) != 1 || !send_wheel(combo))
		return 1;

	combo.setFocus(Qt::OtherFocusReason);
	application.processEvents();
	if (!combo.hasFocus() || !send_wheel(combo))
		return 2;

	return 0;
}
