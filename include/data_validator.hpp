#pragma once

#include <QObject>
#include <QString>
#include <atomic>

class DataValidator final : public QObject
{
    Q_OBJECT
public:
    explicit DataValidator(QString metadataPath, QObject* parent = nullptr);
    void requestCancel() noexcept;

public slots:
    void run();

signals:
    void progress(int percent);
    void finished(bool success, const QString& summary, const QString& reportPath);

private:
    QString metadataPath_;
    std::atomic_bool cancelRequested_{false};
};
