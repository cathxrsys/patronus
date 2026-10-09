#include "audiodevicemanager.h"

#include <QAudioDevice>

#include "settingsmanager.h"

namespace {

const QString kAudioInputSettingName = QStringLiteral("audioInputDeviceId");
const QString kAudioInputDescriptionSettingName =
    QStringLiteral("audioInputDeviceDescription");

}  // namespace

AudioDeviceManager::AudioDeviceManager(SettingsManager* settingsManager,
                                       QObject* parent)
    : QObject(parent), m_settingsManager(settingsManager) {
  connect(&m_mediaDevices, &QMediaDevices::audioInputsChanged, this,
          &AudioDeviceManager::refreshInputDevices);

  if (m_settingsManager != nullptr) {
    connect(m_settingsManager, &SettingsManager::settingChanged, this,
            [this](const QString& settingName, const QString& value) {
              if (settingName != kAudioInputSettingName) {
                return;
              }

              if (m_selectedInputId == value) {
                return;
              }

              m_selectedInputId = value;
              emit selectedInputChanged();
            });
  }

  rebuildInputDevices();
}

QVariantList AudioDeviceManager::inputDevices() const {
  QVariantList result;
  result.reserve(m_inputDevices.size());

  for (const InputDeviceEntry& entry : m_inputDevices) {
    QVariantMap item;
    item.insert(QStringLiteral("id"), entry.id);
    item.insert(QStringLiteral("description"), entry.description);
    item.insert(QStringLiteral("isDefault"), entry.isDefault);
    result.append(item);
  }

  return result;
}

int AudioDeviceManager::selectedInputIndex() const {
  return findSelectedIndex(m_selectedInputId);
}

QString AudioDeviceManager::selectedInputId() const {
  return m_selectedInputId;
}

QString AudioDeviceManager::selectedInputDescription() const {
  const int index = selectedInputIndex();
  if (index < 0 || index >= m_inputDevices.size()) {
    return QString();
  }

  return m_inputDevices.at(index).description;
}

QAudioDevice AudioDeviceManager::selectedAudioInput() const {
  const int index = selectedInputIndex();
  if (index <= 0 || index >= m_inputDevices.size()) {
    return QMediaDevices::defaultAudioInput();
  }

  return m_inputDevices.at(index).device;
}

void AudioDeviceManager::refreshInputDevices() { rebuildInputDevices(); }

void AudioDeviceManager::setSelectedInputIndex(int index) {
  if (index < 0 || index >= m_inputDevices.size()) {
    return;
  }

  setSelectedInputId(m_inputDevices.at(index).id);
}

void AudioDeviceManager::setSelectedInputId(const QString& inputId) {
  if (m_selectedInputId == inputId) {
    return;
  }

  m_selectedInputId = inputId;
  if (m_settingsManager != nullptr) {
    m_settingsManager->setTextSetting(kAudioInputSettingName, inputId);
    const int selectedIndex = findSelectedIndex(inputId);
    const QString description =
        selectedIndex >= 0 && selectedIndex < m_inputDevices.size()
            ? m_inputDevices.at(selectedIndex).description
            : QString();
    m_settingsManager->setTextSetting(kAudioInputDescriptionSettingName,
                                      description);
  }
  emit selectedInputChanged();
}

QString AudioDeviceManager::persistedInputId() const {
  if (m_settingsManager == nullptr) {
    return QString();
  }

  return m_settingsManager->getTextSetting(kAudioInputSettingName, QString());
}

QString AudioDeviceManager::persistedInputDescription() const {
  if (m_settingsManager == nullptr) {
    return QString();
  }

  return m_settingsManager->getTextSetting(kAudioInputDescriptionSettingName,
                                           QString());
}

void AudioDeviceManager::rebuildInputDevices() {
  const QString previousSelectedId = m_selectedInputId;
  const QString persistedId = persistedInputId();
  const QString persistedDescription = persistedInputDescription();
  const QAudioDevice defaultDevice = QMediaDevices::defaultAudioInput();

  m_inputDevices.clear();

  InputDeviceEntry defaultEntry;
  defaultEntry.id = QString();
  defaultEntry.description =
      tr("System default (%1)").arg(defaultDevice.description());
  defaultEntry.device = defaultDevice;
  defaultEntry.isDefault = true;
  m_inputDevices.append(defaultEntry);

  const QList<QAudioDevice> devices = QMediaDevices::audioInputs();
  for (const QAudioDevice& device : devices) {
    InputDeviceEntry entry;
    entry.id = encodeDeviceId(device.id());
    entry.description = device.description();
    entry.device = device;
    entry.isDefault = (device.id() == defaultDevice.id());
    m_inputDevices.append(entry);
  }

  QString resolvedSelectedId = persistedId;
  if (findSelectedIndex(resolvedSelectedId) < 0) {
    const int descriptionIndex =
        findDeviceIndexByDescription(persistedDescription);
    resolvedSelectedId = descriptionIndex >= 0
                             ? m_inputDevices.at(descriptionIndex).id
                             : QString();
  }

  m_selectedInputId = resolvedSelectedId;

  emit inputDevicesChanged();
  if (previousSelectedId != m_selectedInputId) {
    emit selectedInputChanged();
  }
}

int AudioDeviceManager::findSelectedIndex(const QString& inputId) const {
  for (int index = 0; index < m_inputDevices.size(); ++index) {
    if (m_inputDevices.at(index).id == inputId) {
      return index;
    }
  }

  return m_inputDevices.isEmpty() ? -1 : 0;
}

int AudioDeviceManager::findDeviceIndexByDescription(
    const QString& description) const {
  if (description.isEmpty()) {
    return -1;
  }

  for (int index = 0; index < m_inputDevices.size(); ++index) {
    if (m_inputDevices.at(index).description == description) {
      return index;
    }
  }

  return -1;
}

QString AudioDeviceManager::encodeDeviceId(const QByteArray& id) const {
  return QString::fromUtf8(id.toBase64(QByteArray::Base64UrlEncoding |
                                       QByteArray::OmitTrailingEquals));
}