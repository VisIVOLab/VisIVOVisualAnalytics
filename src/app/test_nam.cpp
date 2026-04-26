#include <QNetworkAccessManager>
#include <memory>
thread_local std::unique_ptr<QNetworkAccessManager> nam;
int main() {}
