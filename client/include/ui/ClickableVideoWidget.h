#pragma once

#include <QWidget>
#include <memory>

class QFrame;
class QEnterEvent;
class QEvent;
class QLabel;
class QMouseEvent;
class QResizeEvent;
class QShowEvent;
class QString;
class QVBoxLayout;

class ClickableVideoWidget : public QWidget {
    Q_OBJECT

public:
    explicit ClickableVideoWidget(QWidget* parent = nullptr);
    void setChannelName(const QString& channelName);
    void setExpandedView(bool expanded);

public slots:
    void setLoading(bool loading);
    void refreshChannelLabel();

signals:
    void doubleClicked(ClickableVideoWidget* widget);
    void hoverChanged(bool hovered);

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#endif

private:
    void emitDoubleClickedOnce();
    void setHoverHighlighted(bool highlighted);
    void updateOverlayGeometry();

private:
    std::shared_ptr<QFrame> loadingOverlay_;
    std::shared_ptr<QWidget> loadingSpinner_;
    std::shared_ptr<QLabel> loadingLabel_;
    std::shared_ptr<QLabel> channelLabel_;
    std::shared_ptr<QVBoxLayout> loadingLayout_;

    qint64 lastDoubleClickMsec_ = 0;
    bool hovered_ = false;
    bool expandedView_ = false;
};
