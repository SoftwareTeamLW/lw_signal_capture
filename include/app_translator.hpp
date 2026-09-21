#pragma once

#include <QTranslator>
#include <QString>
#include <atomic>

enum class AppLanguage
{
    Chinese = 0,
    English = 1,
    Russian = 2
};

class AppTranslator final : public QTranslator
{
public:
    explicit AppTranslator(QObject* parent = nullptr);

    void setLanguage(AppLanguage language) { language_.store(static_cast<int>(language), std::memory_order_relaxed); }
    AppLanguage language() const { return static_cast<AppLanguage>(language_.load(std::memory_order_relaxed)); }

    QString translate(const char* context,
                      const char* sourceText,
                      const char* disambiguation = nullptr,
                      int n = -1) const override;

private:
    std::atomic<int> language_{static_cast<int>(AppLanguage::Chinese)};
};
