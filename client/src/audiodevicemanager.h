#pragma once

#include <QAudioDevice>
#include <QMediaDevices>
#include <QObject>
#include <QVariantList>

class SettingsManager;

class AudioDeviceManager : public QObject {
  Q_OBJECT

  Q_PROPERTY(
      QVariantList inputDevices READ inputDevices NOTIFY inputDevicesChanged)
  Q_PROPERTY(int selectedInputIndex READ selectedInputIndex NOTIFY
                 selectedInputChanged)
  Q_PROPERTY(
      QString selectedInputId READ selectedInputId NOTIFY selectedInputChanged)
  Q_PROPERTY(QString selectedInputDescription READ selectedInputDescription
                 NOTIFY selectedInputChanged)

 public:
  explicit AudioDeviceManager(SettingsManager* settingsManager,
                              QObject* parent = nullptr);

  QVariantList inputDevices() const;
  int selectedInputIndex() const;
  QString selectedInputId() const;
  QString selectedInputDescription() const;
  QAudioDevice selectedAudioInput() const;

  Q_INVOKABLE void refreshInputDevices();
  Q_INVOKABLE void setSelectedInputIndex(int index);
  Q_INVOKABLE void setSelectedInputId(const QString& inputId);

 signals:
  void inputDevicesChanged();
  void selectedInputChanged();

 private:
  struct InputDeviceEntry {
    QString id;
    QString description;
    QAudioDevice device;
    bool isDefault = false;
  };

  QString persistedInputId() const;
  QString persistedInputDescription() const;
  void rebuildInputDevices();
  int findSelectedIndex(const QString& inputId) const;
  int findDeviceIndexByDescription(const QString& description) const;
  QString encodeDeviceId(const QByteArray& id) const;

  SettingsManager* m_settingsManager = nullptr;
  QMediaDevices m_mediaDevices;
  QList<InputDeviceEntry> m_inputDevices;
  QString m_selectedInputId;
};