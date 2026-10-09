#pragma once

#include <QObject>

class CoreCryptoAdapter : public QObject {
  Q_OBJECT
 public:
  explicit CoreCryptoAdapter(QObject *parent = nullptr);

  Q_INVOKABLE QString generate_mnemonicphrase();
};