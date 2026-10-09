#include "corecryptoadapter.h"

#include <corecrypto.h>

CoreCryptoAdapter::CoreCryptoAdapter(QObject *parent) : QObject(parent) {}

QString CoreCryptoAdapter::generate_mnemonicphrase() {
  CoreCrypto corecrypto;
  QString mnemonic =
      QString::fromStdString(corecrypto.generate_mnemonicphrase());
  return mnemonic;
}