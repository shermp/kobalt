#pragma once

#include "publication_endpoint.h"
#include "resource_source.h"
#include "source_lease.h"
#include "web_resources.h"

namespace kobalt {
// The factory owns values only. Each ZIP stream is opened, read and closed on
// the engine worker, with cancellation checked before every input block.
ResourceFactory localResourceFactory(QString path, std::shared_ptr<SourceLease> lease);

// Standalone WebKit admission. Run on the resource worker and keep the lease
// for the life of the scope. The local file identity is pinned at admission.
std::unique_ptr<WebResources> openLocalWebResources(const QString &path,
                                                    std::shared_ptr<SourceLease> lease,
                                                    QString *error = nullptr);

namespace native {
// Inspect the unencrypted archive before accepting the opening. The endpoint
// owns no socket or open file; its resources use the existing cache contract.
std::unique_ptr<PublicationEndpoint> openLocalPublication(const QString &path,
                                                          QString *error = nullptr);
} // namespace native
} // namespace kobalt
