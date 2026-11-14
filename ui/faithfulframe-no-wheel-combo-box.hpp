#pragma once

#include <QComboBox>
#include <QWheelEvent>

class FaithfulFrameNoWheelComboBox final : public QComboBox {
public:
	explicit FaithfulFrameNoWheelComboBox(QWidget *parent = nullptr) : QComboBox(parent)
	{
		setFocusPolicy(Qt::StrongFocus);
	}

protected:
	void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};
