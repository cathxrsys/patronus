#pragma once

#include <QEvent>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QWheelEvent>
#include <QtQml>
#include <algorithm>

// Snappy mouse-wheel scrolling for a Flickable / ScrollView on desktop.
//
// Qt turns every mouse-wheel notch into a small physics "flick" that decelerates
// over a few hundred milliseconds, so a wheel feels floaty, moves in tiny steps
// and lags when spun quickly (each notch restarts the deceleration instead of
// adding up). A pure-QML WheelHandler cannot fix this: Flickable installs its own
// event filter that swallows wheel events coming from its children before any
// handler sees them. So we do exactly what Flickable does — install an event
// filter directly on the Flickable — but ours runs first and turns each notch
// into an immediate, fixed-size jump.
//
// Touchpad / high-resolution pixel scrolling (and therefore Android touch) is
// left untouched: those arrive as synthesized events which we pass straight
// through to Flickable's native smooth handling.
//
//     ScrollView {
//         id: view
//         WheelScroller { flickable: view.contentItem }   // contentItem == internal Flickable
//         ColumnLayout { ... }
//     }
class WheelScroller : public QObject {
  Q_OBJECT
  Q_PROPERTY(QQuickItem* flickable READ flickable WRITE setFlickable NOTIFY
                 flickableChanged)
  Q_PROPERTY(qreal step READ step WRITE setStep NOTIFY stepChanged)

 public:
  explicit WheelScroller(QObject* parent = nullptr) : QObject(parent) {}

  QQuickItem* flickable() const { return m_flickable; }
  void setFlickable(QQuickItem* flickable) {
    if (m_flickable == flickable) {
      return;
    }
    if (m_flickable) {
      m_flickable->removeEventFilter(this);
    }
    m_flickable = flickable;
    if (m_flickable) {
      m_flickable->installEventFilter(this);
    }
    emit flickableChanged();
  }

  // How far one wheel notch scrolls, in pixels.
  qreal step() const { return m_step; }
  void setStep(qreal step) {
    if (qFuzzyCompare(m_step, step)) {
      return;
    }
    m_step = step;
    emit stepChanged();
  }

 signals:
  void flickableChanged();
  void stepChanged();
  // Emitted after the wheel actually changed the content position. Lets callers
  // keep e.g. a "stick to bottom" flag in sync (Flickable's onMovementEnded /
  // onDraggingChanged never fire for a wheel-driven direct move).
  void scrolled();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched != m_flickable || event->type() != QEvent::Wheel) {
      return QObject::eventFilter(watched, event);
    }

    auto* wheel = static_cast<QWheelEvent*>(event);

    // Only claim real mouse wheels. Touchpads / high-res gestures arrive as
    // synthesized events and already scroll smoothly; leaving them to Flickable
    // also keeps Wayland touchpad and Android touch behaving natively.
    if (wheel->source() != Qt::MouseEventNotSynthesized) {
      return QObject::eventFilter(watched, event);
    }

    const int angle = wheel->angleDelta().y();
    if (angle == 0) {
      return QObject::eventFilter(watched, event);  // horizontal-only wheel
    }

    const qreal contentHeight = m_flickable->property("contentHeight").toReal();
    const qreal viewportHeight = m_flickable->height();
    const qreal span = contentHeight - viewportHeight;
    if (span <= 0) {
      return QObject::eventFilter(watched, event);  // nothing to scroll
    }

    const qreal originY = m_flickable->property("originY").toReal();
    const qreal contentY = m_flickable->property("contentY").toReal();
    // angleDelta is in 1/8-degree units; 120 == one notch.
    const qreal target =
        std::clamp(contentY - (angle / 120.0) * m_step, originY, originY + span);

    if (!qFuzzyCompare(target, contentY)) {
      // Kill any in-flight flick so consecutive notches don't fight it.
      QMetaObject::invokeMethod(m_flickable, "cancelFlick");
      m_flickable->setProperty("contentY", target);
      emit scrolled();
    }
    return true;  // consume, so Flickable's own smooth-scroll never runs
  }

 private:
  QPointer<QQuickItem> m_flickable;
  qreal m_step = 75.0;
};
