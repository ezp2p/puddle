#include "jsonlistmodel.h"

JsonListModel::JsonListModel(const QStringList &roles, QObject *parent, const QString &keyField)
    : QAbstractListModel(parent), roles_(roles), key_(keyField) {}

int JsonListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(items_.size());
}

QVariant JsonListModel::data(const QModelIndex &index, int role) const
{
    const int r = role - Qt::UserRole;
    if (!index.isValid() || index.row() >= items_.size() || r < 0 || r >= roles_.size())
        return QVariant();
    return items_[index.row()].value(roles_[r]).toVariant();
}

QHash<int, QByteArray> JsonListModel::roleNames() const
{
    QHash<int, QByteArray> h;
    for (int i = 0; i < roles_.size(); i++)
        h[Qt::UserRole + i] = roles_[i].toUtf8();
    return h;
}

void JsonListModel::setItems(const QList<QJsonObject> &items)
{
    auto same = [this](const QJsonObject &a, const QJsonObject &b) { return a.value(key_) == b.value(key_); };
    const qsizetype oldN = items_.size(), newN = items.size();
    const int before = int(oldN);

    // Where do the rows we have sit in the new list? Only a single unbroken run counts.
    qsizetype at = -1;
    if (oldN > 0 && newN >= oldN) {
        for (qsizetype k = 0; k + oldN <= newN && at < 0; k++) {
            bool run = true;
            for (qsizetype i = 0; run && i < oldN; i++)
                run = same(items[k + i], items_[i]);
            if (run)
                at = k;
        }
    }
    if (oldN == 0 && newN == 0)
        return;
    if (at < 0) {
        beginResetModel();
        items_ = items;
        endResetModel();
    } else {
        if (at > 0) {
            beginInsertRows(QModelIndex(), 0, int(at) - 1);
            for (qsizetype i = 0; i < at; i++)
                items_.insert(i, items[i]);
            endInsertRows();
        }
        const qsizetype tail = newN - at - oldN;
        for (qsizetype i = 0; i < oldN; i++) {
            if (items[at + i] == items_[at + i])
                continue;
            items_[at + i] = items[at + i];
            emit dataChanged(index(int(at + i)), index(int(at + i)));
        }
        if (tail > 0) {
            beginInsertRows(QModelIndex(), int(at + oldN), int(newN) - 1);
            for (qsizetype i = at + oldN; i < newN; i++)
                items_.append(items[i]);
            endInsertRows();
        }
    }
    if (before != items_.size())
        emit countChanged();
}
