#pragma once

#include <QAbstractListModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

// A list of JSON objects for QML. Each name in `roles` is a role that reads that member of the object (missing = empty).
// The key member ("id" unless told otherwise) tells which row is which, so a refresh that changes little changes few rows:
// rows added before or after the ones already there are inserted, and the rest are updated in place.
class JsonListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    explicit JsonListModel(const QStringList &roles, QObject *parent = nullptr, const QString &keyField = QStringLiteral("id"));

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QJsonObject at(int row) const { return row >= 0 && row < items_.size() ? items_[row] : QJsonObject(); }
    const QList<QJsonObject> &items() const { return items_; }

    // Make the list these items. Rows with the same id in the same place are updated, not replaced.
    void setItems(const QList<QJsonObject> &items);
    void clear() { setItems({}); }

signals:
    void countChanged();

private:
    QStringList roles_;
    QString key_;
    QList<QJsonObject> items_;
};
