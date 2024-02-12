#ifndef PVDOWNLOADMANAGER_H
#define PVDOWNLOADMANAGER_H

#include <QObject>

class pvDownloadManager : public QObject
{
    Q_OBJECT
public:
    pvDownloadManager();

signals:

private:
    //pqServer* server;
};

#endif // PVDOWNLOADMANAGER_H
