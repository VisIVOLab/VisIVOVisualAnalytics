#ifndef DatasetWindowFactory_h
#define DatasetWindowFactory_h

#include "DatasetOpenTypes.h"

class QWidget;
class QString;

class DatasetWindowFactory
{
public:
    QWidget *createWindow(const DatasetOpenInfo &dataset, const QString &backendUrl = QString(),
                          const QString &backendToken = QString(),
                          QWidget *parent = nullptr) const;
};

#endif
