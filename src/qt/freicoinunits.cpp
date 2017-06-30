#include "xunits.h"

#include <QStringList>

XcoinUnits::XcoinUnits(QObject *parent):
        QAbstractListModel(parent),
        unitlist(availableUnits())
{
}

QList<XcoinUnits::Unit> XcoinUnits::availableUnits()
{
    QList<XcoinUnits::Unit> unitlist;
    unitlist.append(XCN);
    unitlist.append(mXCN);
    unitlist.append(uXCN);
    return unitlist;
}

bool XcoinUnits::valid(int unit)
{
    switch(unit)
    {
    case XCN:
    case mXCN:
    case uXCN:
        return true;
    default:
        return false;
    }
}

QString XcoinUnits::name(int unit)
{
    switch(unit)
    {
    case XCN: return QString("XCN");
    case mXCN: return QString("mXCN");
    case uXCN: return QString::fromUtf8("μXCN");
    default: return QString("???");
    }
}

QString XcoinUnits::description(int unit)
{
    switch(unit)
    {
    case XCN: return QString("Xcoins");
    case mXCN: return QString("Milli-Xcoins (1 / 1,000)");
    case uXCN: return QString("Micro-Xcoins (1 / 1,000,000)");
    default: return QString("???");
    }
}

mpq XcoinUnits::factor(int unit)
{
    switch(unit)
    {
    case uXCN: return mpq("100/1");
    case mXCN: return mpq("100000/1");
    default:
    case XCN:  return mpq("100000000/1");
    }
}

int XcoinUnits::amountDigits(int unit)
{
    switch(unit)
    {
    case XCN: return 8; // <100,000,000 (# digits, without commas)
    case mXCN: return 11; // <100,000,000,000
    case uXCN: return 14; // <100,000,000,000,000
    default: return 0;
    }
}

int XcoinUnits::decimals(int unit)
{
    switch(unit)
    {
    case XCN: return 8;
    case mXCN: return 5;
    case uXCN: return 2;
    default: return 0;
    }
}

QString XcoinUnits::format(int unit, const mpq& n, bool fPlus)
{
    // Note: not using straight sprintf here because we do NOT want
    // localized number formatting.
    if(!valid(unit))
        return QString(); // Refuse to format invalid unit
    mpq q = n * COIN / factor(unit);
    std::string str = FormatMoney(q, fPlus);
    int diff = 8 - decimals(unit);
    if(diff > 0)
        str.erase(str.length() - diff, diff);
    return QString::fromStdString(str);
}

QString XcoinUnits::formatWithUnit(int unit, const mpq& amount, bool plussign)
{
    return format(unit, amount, plussign) + QString(" ") + name(unit);
}

bool XcoinUnits::parse(int unit, const QString &value, mpq *val_out)
{
    mpq ret_value;
    if (!ParseMoney(value.toStdString(), ret_value))
        return false;
    if(val_out)
    {
        *val_out = ret_value * factor(unit) / COIN;
    }
    return true;
}

int XcoinUnits::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return unitlist.size();
}

QVariant XcoinUnits::data(const QModelIndex &index, int role) const
{
    int row = index.row();
    if(row >= 0 && row < unitlist.size())
    {
        Unit unit = unitlist.at(row);
        switch(role)
        {
        case Qt::EditRole:
        case Qt::DisplayRole:
            return QVariant(name(unit));
        case Qt::ToolTipRole:
            return QVariant(description(unit));
        case UnitRole:
            return QVariant(static_cast<int>(unit));
        }
    }
    return QVariant();
}
