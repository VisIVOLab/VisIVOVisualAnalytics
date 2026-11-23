#ifndef TestParticleLightReport_h
#define TestParticleLightReport_h

#include <QObject>

class TestParticleLightReport : public QObject
{
    Q_OBJECT

private slots:
    void headersAndTaskResultsAreRead();
    void theLineAndTooltipSayWhatWasLeftOut();
    void aTypeColumnIsFound();
    void gasOnlyIsSuggestedWhenTwoPopulationsShareTheLight();
    void periodIsSentOnlyWhenTheUserTurnsItOff();
};

#endif
