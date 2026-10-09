#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "corecrypto.h"

constexpr const char* VISUAL_DELIMITER =
    "=========================================";

int main(int argc, char* argv[]) {
  std::string mnemonic_phrase = CoreCrypto::generate_mnemonicphrase();
  std::vector<uint8_t> seed =
      CoreCrypto::mnemonicphrase_to_seed(mnemonic_phrase);

  struct rlimit rl;
  rl.rlim_cur = 0;
  rl.rlim_max = 0;
  setrlimit(RLIMIT_CORE, &rl);

  std::cout << VISUAL_DELIMITER << "\n\n"
            << "Write mnemonic to paper: \n\n";

  int ttyfd = open("/dev/tty", O_WRONLY);
  if (ttyfd < 0) {
    std::cerr << "Open error /dev/tty, output to stdout\n";
    std::cout << mnemonic_phrase << std::endl;
  } else {
    write(ttyfd, mnemonic_phrase.c_str(), mnemonic_phrase.size());
    write(ttyfd, "\n", 1);
  }

  std::cout << "\n\n" << VISUAL_DELIMITER << std::endl;

  std::cout << "Input something when you wrote down the mnemonic: ";
  std::cin >> std::ws;

  const char* clear_seq = "\033[H\033[2J\033[3J";
  if (ttyfd >= 0) {
    write(ttyfd, clear_seq, strlen(clear_seq));
  } else {
    std::cout << clear_seq << std::flush;
  }

  volatile char* p = const_cast<volatile char*>(mnemonic_phrase.data());
  for (size_t i = 0; i < mnemonic_phrase.size(); ++i) p[i] = '\0';
  mnemonic_phrase.clear();

  CoreCrypto::secure_memory_zero(mnemonic_phrase);

  if (ttyfd >= 0) close(ttyfd);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  CoreCrypto::SignKeyPair keypair = CoreCrypto::seed_to_keypair(seed);
  CoreCrypto::secure_memory_zero(seed);

  //    std::cout << "Derived privkey: " << CoreCrypto::to_hex(keypair.privkey)
  //    << std::endl;

  std::cout << VISUAL_DELIMITER << "\n\nPubKey fingerprint: "
            << CoreCrypto::fingerprint(keypair.pubkey) << std::endl;

  std::string priv_b64 =
      CoreCrypto::to_base64(keypair.privkey.data(), keypair.privkey.size());
  std::string pub_b64 =
      CoreCrypto::to_base64(keypair.pubkey.data(), keypair.pubkey.size());

  std::string json;
  {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"privkey\": \"" << priv_b64 << "\",\n"
        << "  \"pubkey\":  \"" << pub_b64 << "\"\n"
        << "}\n";
    json = oss.str();
  }

  const char* filename = "certificate.json";
  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC,
                S_IRUSR | S_IWUSR);  // mode 0600
  if (fd < 0) {
    std::cerr << "Create error " << filename << ": " << strerror(errno)
              << std::endl;
  } else {
    ssize_t wrote = write(fd, json.data(), json.size());
    if (wrote < 0 || static_cast<size_t>(wrote) != json.size()) {
      std::cerr << "Write error " << filename << ": " << strerror(errno)
                << std::endl;
      close(fd);
    } else {
      fsync(fd);
      close(fd);
      std::cout << "Saved certificate to " << filename << " (mode 0600).\n\n"
                << VISUAL_DELIMITER << std::endl;
    }
  }

  CoreCrypto::secure_memory_zero(keypair.privkey);
  CoreCrypto::secure_memory_zero(keypair.pubkey);
  CoreCrypto::secure_memory_zero(priv_b64);
  CoreCrypto::secure_memory_zero(pub_b64);
  CoreCrypto::secure_memory_zero(json);

  return 0;
}