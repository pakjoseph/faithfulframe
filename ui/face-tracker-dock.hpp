#pragma once

#ifdef __cplusplus
#include <QFrame>
#include <QElapsedTimer>
#include <QPointF>
#include <QString>
#include <memory>
#include <string>
#include <obs.h>
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <util/platform.h>

namespace faithfulframe::performance {
class csv_spool;
}

class FTDock : public QFrame {
	Q_OBJECT

public:
	class FTWidget *widget;
	std::string name;
	struct face_tracker_dock_s *data;

	class QVBoxLayout *mainLayout;
	class QComboBox *targetSelector;
	class QPushButton *pauseButton;
	class QPushButton *resetButton;
	class QPushButton *enableButton;
	class QPushButton *propertyButton;
	class FTWidget *ftWidget;
	class QCheckBox *notrackButton;
	class QLabel *safeStatusLabel;
	class QToolButton *performanceToggle;
	class QLabel *performanceLabel;
	class QLineEdit *performanceRunNotes;
	class QPushButton *performanceRecordButton;
	class QComboBox *candidateSelector;
	class QPushButton *autoFollowButton;
	class QPushButton *manualOverrideButton;
	class QPushButton *selectCandidateButton;
	class QTimer *statusTimer;
	os_cpu_usage_info_t *processCpuUsageInfo = nullptr;
	QElapsedTimer processMetricsTimer;
	double processCpuPercent = -1.0;
	os_proc_memory_usage_t processMemoryUsage{};
	bool processMemoryAvailable = false;
	bool performanceRecording = false;
	uint64_t performanceRecordingSourceId = 0;
	uint64_t performanceRecordingSessionId = 0;
	QElapsedTimer performanceRecordingTimer;
	std::unique_ptr<faithfulframe::performance::csv_spool> performanceSpool;
	uint64_t performanceSampleCount = 0;
	QString operatorNotice;
	QString performanceHardwareAndWorkloadNotes;
	QElapsedTimer operatorNoticeTimer;

	bool updating_widget = false;
	bool in_updateState = false;
	bool design_tokens_ready = false;

public:
	FTDock(QWidget *parent = nullptr);
	~FTDock();
	void closeEvent(QCloseEvent *event) override;
	void changeEvent(QEvent *event) override;

	static void default_properties(obs_data_t *);
	void save_properties(obs_data_t *);
	void load_properties(obs_data_t *);

private:
	void applyDesignTokens();
	void showOperatorNotice(const QString &message);
	void stopPerformanceMetrics();
	bool exportPerformanceRecording();
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

	OBSSource get_source();
	signal_handler_t *source_sh = NULL;

	static void onStateChanged(void *data, calldata_t *cd);

signals:
	void scenesMayChanged();
	void dataChanged();

public slots:
	void checkTargetSelector();
	void updateState();
	void updateWidget();
	void removeDock();

private slots:
	void targetSelectorChanged();
	void pauseButtonClicked(bool checked);
	void resetButtonClicked(bool checked);
	void enableButtonClicked(bool checked);
	void propertyButtonClicked(bool checked);
	void notrackButtonChanged(
#if QT_VERSION < QT_VERSION_CHECK(6, 7, 0)
		int state
#else
		Qt::CheckState state
#endif
	);
	void autoFollowButtonClicked();
	void manualOverrideButtonClicked();
	void selectCandidateButtonClicked();
	void handlePreviewClick(const QPointF &position);
	void performanceRecordButtonClicked();
};

extern "C" {
#endif // __cplusplus
void ft_dock_add(const char *name, obs_data_t *props, bool show);
void ft_docks_init();
void ft_docks_release();

#ifdef __cplusplus
} // extern "C"
#endif
