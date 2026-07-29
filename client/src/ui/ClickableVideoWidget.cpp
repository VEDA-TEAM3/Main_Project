#include "ui/ClickableVideoWidget.h"

#include <QDateTime>
#include <QEnterEvent>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr qint64 doubleClickDebounceMsec = 250;

/**
 * @brief   영상 로딩 중 중앙에 표시되는 회전 인디케이터 위젯입니다.
 */
class SpinnerWidget : public QWidget {
public:
    /**
     * @brief       스피너 크기와 회전 타이머를 초기화합니다.
     * @param parent  부모 위젯
     */
    explicit SpinnerWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(44, 44);

        timer_.setInterval(80);

        connect(&timer_, &QTimer::timeout, this, [this]() {
            angle_ = (angle_ + 30) % 360;
            update();
        });
    }

    /**
     * @brief          스피너 회전 상태를 변경합니다.
     * @param spinning  true이면 회전 타이머를 시작합니다.
     */
    void setSpinning(bool spinning) {
        if (spinning) {
            timer_.start();
        } else {
            timer_.stop();
        }

        update();
    }

protected:
    /**
     * @brief   현재 회전 각도에 맞춰 원형 로딩 표시를 그립니다.
     */
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF ring = rect().adjusted(6, 6, -6, -6);

        QPen basePen(QColor(47, 78, 92), 4, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(basePen);
        painter.drawArc(ring, 0, 360 * 16);

        QPen accentPen(QColor(90, 194, 227), 4, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(accentPen);
        painter.drawArc(ring, angle_ * 16, 115 * 16);
    }

private:
    QTimer timer_;
    int angle_ = 0;
};
}  // namespace

/**
 * @brief       네이티브 영상 출력 영역과 로딩 오버레이를 초기화합니다.
 * @param parent  부모 위젯
 */
ClickableVideoWidget::ClickableVideoWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setProperty("loading", true);

    loadingOverlay_ = std::make_shared<QFrame>(this);
    loadingOverlay_->setObjectName("videoLoadingOverlay");
    loadingOverlay_->setAttribute(Qt::WA_TransparentForMouseEvents);

    loadingLayout_ = std::make_shared<QVBoxLayout>(loadingOverlay_.get());
    loadingLayout_->setContentsMargins(16, 16, 16, 16);
    loadingLayout_->setSpacing(10);
    loadingLayout_->setAlignment(Qt::AlignCenter);

    auto spinner = std::make_shared<SpinnerWidget>(loadingOverlay_.get());
    spinner->setObjectName("videoLoadingSpinner");
    loadingSpinner_ = spinner;

    loadingLabel_ = std::make_shared<QLabel>(QStringLiteral("영상 연결 중"), loadingOverlay_.get());
    loadingLabel_->setObjectName("videoLoadingLabel");
    loadingLabel_->setAlignment(Qt::AlignCenter);

    loadingLayout_->addWidget(spinner.get(), 0, Qt::AlignCenter);
    loadingLayout_->addWidget(loadingLabel_.get(), 0, Qt::AlignCenter);

    channelLabel_ = std::make_shared<QLabel>(this);
    channelLabel_->setObjectName("videoChannelLabel");
    channelLabel_->setAttribute(Qt::WA_DontCreateNativeAncestors);
    channelLabel_->setAttribute(Qt::WA_NativeWindow);
    channelLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    channelLabel_->setAlignment(Qt::AlignCenter);
    channelLabel_->setFocusPolicy(Qt::NoFocus);
    channelLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
    channelLabel_->setCursor(Qt::ArrowCursor);
    channelLabel_->setProperty("expanded", false);
    channelLabel_->hide();

    spinner->setSpinning(true);

    updateOverlayGeometry();
}

/**
 * @brief             영상 좌측 상단에 표시할 채널 이름을 설정합니다.
 * @param channelName 표시할 채널 이름
 */
void ClickableVideoWidget::setChannelName(const QString& channelName) {
    if (!channelLabel_) {
        return;
    }

    channelLabel_->setText(channelName);
    channelLabel_->setVisible(!channelName.isEmpty());
    updateOverlayGeometry();
}

/**
 * @brief         영상 확대 여부에 맞춰 채널 라벨 크기를 변경합니다.
 * @param expanded 확대 화면이면 true
 */
void ClickableVideoWidget::setExpandedView(bool expanded) {
    if (!channelLabel_ || expandedView_ == expanded) {
        return;
    }

    expandedView_ = expanded;
    channelLabel_->setProperty("expanded", expandedView_);
    channelLabel_->style()->unpolish(channelLabel_.get());
    channelLabel_->style()->polish(channelLabel_.get());
    updateOverlayGeometry();
}

/**
 * @brief         스트림 로딩 오버레이의 표시 상태를 변경합니다.
 * @param loading  true이면 로딩 오버레이와 스피너를 표시합니다.
 */
void ClickableVideoWidget::setLoading(bool loading) {
    setProperty("loading", loading);

    if (loadingOverlay_) {
        loadingOverlay_->setVisible(loading);
        loadingOverlay_->raise();
    }

    refreshChannelLabel();

    if (loadingSpinner_) {
        static_cast<SpinnerWidget*>(loadingSpinner_.get())->setSpinning(loading);
    }

    style()->unpolish(this);
    style()->polish(this);
    update();
}

/** @brief 영상 표면 재생성 뒤에도 채널 라벨을 네이티브 윈도우 최상단으로 복구합니다. */
void ClickableVideoWidget::refreshChannelLabel() {
    if (!channelLabel_ || channelLabel_->text().isEmpty()) {
        return;
    }

    channelLabel_->show();
    channelLabel_->raise();

#ifdef Q_OS_WIN
    const HWND labelWindow = reinterpret_cast<HWND>(channelLabel_->winId());
    if (labelWindow) {
        SetWindowPos(labelWindow, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
#endif
}

/**
 * @brief       마우스가 영상 영역에 들어오면 선택 가능 상태를 테두리로 표시합니다.
 * @param event Qt 진입 이벤트
 */
void ClickableVideoWidget::enterEvent(QEnterEvent* event) {
    QWidget::enterEvent(event);
    setHoverHighlighted(true);
}

/**
 * @brief       마우스가 영상 영역을 벗어나면 hover 테두리를 숨깁니다.
 * @param event Qt 이탈 이벤트
 */
void ClickableVideoWidget::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    setHoverHighlighted(false);
}

/**
 * @brief       Qt 더블클릭 이벤트를 수신해 중복 방지 후 시그널을 발생시킵니다.
 * @param event  마우스 더블클릭 이벤트
 */
void ClickableVideoWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    emitDoubleClickedOnce();
    event->accept();
}

/**
 * @brief       위젯 크기 변경 시 로딩 오버레이를 영상 영역에 맞춥니다.
 * @param event  Qt resize 이벤트
 */
void ClickableVideoWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateOverlayGeometry();
}

/** @brief 위젯이 다시 표시될 때 채널 라벨의 네이티브 Z 순서를 복구합니다. */
void ClickableVideoWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    QTimer::singleShot(0, this, &ClickableVideoWidget::refreshChannelLabel);
}

#ifdef Q_OS_WIN
/**
 * @brief        Windows 네이티브 더블클릭 메시지를 Qt 시그널로 변환합니다.
 * @param eventType  네이티브 이벤트 종류
 * @param message    Windows MSG 포인터
 * @param result     이벤트 처리 결과 저장 위치
 * @return           처리한 메시지이면 true
 */
bool ClickableVideoWidget::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    Q_UNUSED(eventType)
    Q_UNUSED(result)

    MSG* msg = static_cast<MSG*>(message);

    if (msg) {
        if (msg->message == WM_MOUSEMOVE) {
            TRACKMOUSEEVENT trackingEvent{};
            trackingEvent.cbSize = sizeof(trackingEvent);
            trackingEvent.dwFlags = TME_LEAVE;
            trackingEvent.hwndTrack = msg->hwnd;
            TrackMouseEvent(&trackingEvent);
            setHoverHighlighted(true);
        } else if (msg->message == WM_MOUSELEAVE) {
            setHoverHighlighted(false);
        } else if (msg->message == WM_LBUTTONDBLCLK) {
            emitDoubleClickedOnce();
            return true;
        }
    }

    return QWidget::nativeEvent(eventType, message, result);
}
#endif

/**
 * @brief   짧은 시간에 중복 발생한 더블클릭을 걸러낸 뒤 시그널을 발생시킵니다.
 */
void ClickableVideoWidget::emitDoubleClickedOnce() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (now - lastDoubleClickMsec_ < doubleClickDebounceMsec) {
        return;
    }

    lastDoubleClickMsec_ = now;

    emit doubleClicked(this);
}

/**
 * @brief             현재 영상 칸의 hover 테두리 표시 상태를 변경합니다.
 * @param highlighted  표시할 경우 true
 */
void ClickableVideoWidget::setHoverHighlighted(bool highlighted) {
    if (hovered_ == highlighted) {
        return;
    }

    hovered_ = highlighted;
    emit hoverChanged(hovered_);
}

/**
 * @brief   로딩 오버레이를 현재 위젯 영역 전체에 맞추고 최상단으로 올립니다.
 */
void ClickableVideoWidget::updateOverlayGeometry() {
    if (loadingOverlay_) {
        loadingOverlay_->setGeometry(rect());
        loadingOverlay_->raise();
    }

    if (channelLabel_ && channelLabel_->isVisible()) {
        channelLabel_->adjustSize();
        const int margin = expandedView_ ? 16 : 10;
        channelLabel_->clearMask();
        channelLabel_->move(margin, margin);
        refreshChannelLabel();
    }
}
