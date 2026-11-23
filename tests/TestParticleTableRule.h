#ifndef TestParticleTableRule_h
#define TestParticleTableRule_h

#include <QObject>

class TestParticleTableRule : public QObject
{
    Q_OBJECT

private slots:
    void particleExportsAreParticles();
    void cataloguesOfObjectsAreNot();
    void smallTablesStayCatalogues();
    void theUsersChoiceWinsAndIsRememberedPerFile();
};

#endif
