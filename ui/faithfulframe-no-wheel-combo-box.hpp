#pragma once

#include <QComboBox>
#include <QWheelEvent>

#include "faithfulframe-no-wheel-policy.hpp"

class FaithfulFrameNoWheelComboBox final : public QComboBox {
public:
	explicit FaithfulFrameNoWheelComboBox(QWidget *parent = nullptr) : QComboBox(parent)
	{
		setFocusPolicy(faithfulframe::ui::selector_focus_policy);
	}

protected:
	void wheelEvent(QWheelEvent *event) override { faithfulframe::ui::ignore_selector_wheel_event(event); }
};
