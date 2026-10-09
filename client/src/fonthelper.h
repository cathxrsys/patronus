#pragma once

#include <QFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QObject>
#include <QString>
#include <QTextBoundaryFinder>
#include <QtQml>

// Builds fonts/rich text whose glyph-fallback chain ends in the bundled
// color-emoji font ("Noto Color Emoji", registered by the FontLoader in
// Main.qml), so emoji render identically on every platform instead of via
// each OS's own set. Exposed to QML as `Fonts`.
//
// QML's `font` value type only exposes `family` (a single string), not the
// multi-family `families` list, so multi-family fonts have to be built here
// where QFont::setFamilies is available.
class FontHelper : public QObject {
  Q_OBJECT
 public:
  explicit FontHelper(QObject* parent = nullptr) : QObject(parent) {}

  // A font whose fallback chain ends in the bundled emoji font. Use like:
  // `font: Fonts.emoji("Roboto", 14, Font.Light)`.
  //
  // `family` is resolved to its concrete substitute (QFontInfo) before being
  // combined with the emoji family: on systems where `family` itself isn't
  // installed (e.g. "Roboto" on a plain Linux desktop, which substitutes to
  // "Noto Sans"), passing the literal unresolved name into a multi-family
  // QFont lets per-glyph fallback selection skip it, so glyphs that exist in
  // BOTH the substitute and the emoji font (ASCII digits, #, * — the emoji
  // font ships those for keycap sequences) can get drawn by the emoji font
  // instead — e.g. a plain "5" rendering as a little colored tile. Resolving
  // first pins the real family as an unambiguous first choice.
  Q_INVOKABLE QFont emoji(const QString& family, int pixelSize,
                          int weight = -1) const {
    QFont f;
    f.setFamilies({resolvedFamily(family), QStringLiteral("Noto Color Emoji")});
    if (pixelSize > 0) {
      f.setPixelSize(pixelSize);
    }
    if (weight >= 0) {
      f.setWeight(static_cast<QFont::Weight>(weight));
    }
    return f;
  }

  // Rich-text markup for a plain-text message: emoji grapheme clusters are
  // wrapped in a <span> that switches to the bundled emoji font at a larger
  // size (color emoji fonts read visually smaller than Latin text at the same
  // nominal pixel size), everything else stays untouched at the base font/
  // size/color the QML Text item already has. Use with `textFormat:
  // Text.RichText`; NOT supported together with `elide` (Qt Rich Text doesn't
  // support eliding) — for elided text, use `emoji()` above instead.
  //
  // All plain-text runs are HTML-escaped, so arbitrary contact-supplied text
  // can never inject markup; the only tag ever emitted is the fixed <span>
  // wrapper below, never built from caller-controlled attribute values.
  Q_INVOKABLE QString richText(const QString& plain, int baseSizePx,
                               double emojiScale = 1.3) const {
    QString html;
    html.reserve(plain.size() * 2);

    const int emojiSize = qRound(baseSizePx * emojiScale);
    QTextBoundaryFinder bf(QTextBoundaryFinder::Grapheme, plain);
    qsizetype prevPos = 0;
    bool haveRun = false;
    bool prevWasEmoji = false;
    QString runBuf;

    auto flushRun = [&]() {
      if (runBuf.isEmpty()) return;
      QString esc = runBuf.toHtmlEscaped();
      esc.replace(QLatin1String("\n"), QLatin1String("<br/>"));
      if (prevWasEmoji) {
        html += QStringLiteral(
                    "<span style=\"font-family:'Noto Color Emoji'; "
                    "font-size:%1px;\">%2</span>")
                    .arg(emojiSize)
                    .arg(esc);
      } else {
        html += esc;
      }
      runBuf.clear();
    };

    qsizetype pos;
    while ((pos = bf.toNextBoundary()) != -1) {
      const QString cluster = plain.mid(prevPos, pos - prevPos);
      const bool isEmoji = clusterIsEmoji(cluster);
      if (haveRun && isEmoji != prevWasEmoji) {
        flushRun();
      }
      runBuf += cluster;
      prevWasEmoji = isEmoji;
      haveRun = true;
      prevPos = pos;
    }
    flushRun();
    return html;
  }

  // The width of the widest line `richText()` would render for this string,
  // at the same segmentation/sizing (plain runs at `baseSizePx` via
  // "Roboto", emoji runs at `baseSizePx * emojiScale` via the bundled emoji
  // font). Message bubbles size themselves to fit their text via a
  // `TextMetrics`-style measurement of the RAW string in a single plain
  // font/size — that measurement is wildly wrong once emoji are involved
  // (Roboto has no emoji glyphs, so measuring "🎉🎉🎉" through it produces a
  // width nowhere near what richText() actually renders at 1.3x size via a
  // completely different font), so the bubble ends up too narrow and the
  // richText content wraps at seemingly-arbitrary points clustered around
  // the emoji. Use this in place of that measurement.
  Q_INVOKABLE qreal richTextWidth(const QString& plain, int baseSizePx,
                                  double emojiScale = 1.3) const {
    QFont plainFont(resolvedFamily(QStringLiteral("Roboto")));
    plainFont.setPixelSize(baseSizePx);
    const QFontMetricsF plainMetrics(plainFont);

    QFont emojiFontLocal(QStringLiteral("Noto Color Emoji"));
    emojiFontLocal.setPixelSize(qRound(baseSizePx * emojiScale));
    const QFontMetricsF emojiMetrics(emojiFontLocal);

    qreal maxLineWidth = 0;
    qreal currentLineWidth = 0;
    QTextBoundaryFinder bf(QTextBoundaryFinder::Grapheme, plain);
    qsizetype prevPos = 0;
    bool haveRun = false;
    bool prevWasEmoji = false;
    QString runBuf;

    auto flushRun = [&]() {
      if (runBuf.isEmpty()) return;
      currentLineWidth +=
          (prevWasEmoji ? emojiMetrics : plainMetrics).horizontalAdvance(runBuf);
      runBuf.clear();
    };

    qsizetype pos;
    while ((pos = bf.toNextBoundary()) != -1) {
      const QString cluster = plain.mid(prevPos, pos - prevPos);
      if (cluster == QLatin1String("\n")) {
        flushRun();
        maxLineWidth = qMax(maxLineWidth, currentLineWidth);
        currentLineWidth = 0;
        prevWasEmoji = false;
        haveRun = false;
        prevPos = pos;
        continue;
      }
      const bool isEmoji = clusterIsEmoji(cluster);
      if (haveRun && isEmoji != prevWasEmoji) {
        flushRun();
      }
      runBuf += cluster;
      prevWasEmoji = isEmoji;
      haveRun = true;
      prevPos = pos;
    }
    flushRun();
    maxLineWidth = qMax(maxLineWidth, currentLineWidth);
    return maxLineWidth;
  }

 private:
  static QString resolvedFamily(const QString& family) {
    return QFontInfo(QFont(family)).family();
  }

  // Mirrors the block ranges + keep-list used by tools/gen_emoji.py to build
  // the picker's dataset, plus the ZWJ/keycap combiners needed to recognize
  // full sequences (flags, keycaps) — see fonthelper.h's richText() doc.
  static bool isEmojiCodepoint(char32_t cp) {
    static const char32_t keep[] = {
        0x2764, 0x2665, 0x2660, 0x2663, 0x2666, 0x2b50, 0x2b55, 0x2705,
        0x274c, 0x2753, 0x2757, 0x2755, 0x2714, 0x2716, 0x2795, 0x2796,
        0x2797, 0x00a9,  0x00ae, 0x2122, 0x203c, 0x2049, 0x3030, 0x303d,
        0x3297, 0x3299, 0x2b1b, 0x2b1c, 0x25aa, 0x25ab, 0x25fe, 0x25fd,
        0x25fb, 0x25fc, 0x2b06, 0x2b07, 0x2b05, 0x27a1, 0x2934, 0x2935,
        0x21a9, 0x21aa, 0x2194, 0x2195, 0x2196, 0x2197, 0x2198, 0x2199,
    };
    if (cp == 0x200D || cp == 0x20E3) return true;  // ZWJ, keycap combiner
    if (cp >= 0x1F000 && cp <= 0x1FFFF) return true;
    if (cp >= 0x2600 && cp <= 0x27BF) return true;
    for (char32_t k : keep) {
      if (cp == k) return true;
    }
    return false;
  }

  // A grapheme cluster (one or more QChars UAX#29 groups as a single visual
  // unit — e.g. a flag's 2 regional indicators, or digit+FE0F+keycap) counts
  // as emoji if ANY codepoint in it matches, so a genuine keycap sequence
  // (base digit + U+20E3) is recognized via the combiner even though the bare
  // base digit alone is not itself emoji.
  static bool clusterIsEmoji(const QString& cluster) {
    qsizetype i = 0;
    while (i < cluster.size()) {
      char32_t cp;
      if (cluster.at(i).isHighSurrogate() && i + 1 < cluster.size() &&
          cluster.at(i + 1).isLowSurrogate()) {
        cp = QChar::surrogateToUcs4(cluster.at(i), cluster.at(i + 1));
        i += 2;
      } else {
        cp = cluster.at(i).unicode();
        i += 1;
      }
      if (isEmojiCodepoint(cp)) return true;
    }
    return false;
  }
};
