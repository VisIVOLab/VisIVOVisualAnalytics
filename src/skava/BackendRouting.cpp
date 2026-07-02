#include "BackendRouting.h"

BackendNode pickBackendForSkavaDataset(Settings *settings,
                                       const SkavaDatalinkResponse &datalink)
{
    if (!settings || datalink.visivoBackends.isEmpty()) {
        return {};
    }
    const auto registry = settings->getBackendNodes();
    const auto trim = [](QString s) {
        while (s.endsWith(u'/')) s.chop(1);
        return s;
    };
    for (const auto &desc : datalink.visivoBackends) {
        const QString descUrl = trim(desc.endpoint);
        // (1) exact URL match
        for (const auto &node : registry) {
            if (trim(node.url) == descUrl && !descUrl.isEmpty()) {
                return node;
            }
        }
        // (2) node_code match
        if (!desc.nodeCode.isEmpty()) {
            for (const auto &node : registry) {
                if (node.srcCode == desc.nodeCode) {
                    return node;
                }
            }
        }
    }
    return {};
}
