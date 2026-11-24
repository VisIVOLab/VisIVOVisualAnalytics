#include "LinkedDatasetSky.h"

#include "AstroUtils.h"
#include "app/BackendClient.h"

#include <QCursor>
#include <QGuiApplication>

#include <string>
#include <vector>

LinkedDatasetSky::LinkedDatasetSky() = default;
LinkedDatasetSky::~LinkedDatasetSky() = default;

AstroUtils *LinkedDatasetSky::get(const QString &backendUrl, const QString &token,
                                  const QString &sessionId, const QString &filepath)
{
    // A header that could not be fetched is asked for again only after a while: the
    // cursor reaches here on every mouse move, and each ask waits on the network.
    if (!m_read && (!m_failedAt.isValid() || m_failedAt.elapsed() > 10000)) {
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        BackendClient client(backendUrl, token);
        if (!sessionId.isEmpty())
            client.setSessionId(sessionId);
        const auto header = client.fileHeader(filepath);
        QGuiApplication::restoreOverrideCursor();
        if (header.valid) {
            std::vector<std::string> cards;
            cards.reserve(static_cast<std::size_t>(header.cards.size()));
            for (const QString &c : header.cards)
                cards.push_back(c.toStdString());
            auto astro = std::make_unique<AstroUtils>(cards);
            // A header with no celestial solution does not grow one.
            if (astro->hasSky())
                m_astro = std::move(astro);
        }
        // Read once, found or not.
        m_read = header.valid;
        if (!header.valid)
            m_failedAt.start();
    }
    return m_astro.get();
}
