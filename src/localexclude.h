#ifndef LOCALEXCLUDE_H
#define LOCALEXCLUDE_H

#include <QDialog>

namespace Ui {
class LocalExclude;
}

class LocalExclude : public QDialog
{
    Q_OBJECT

public:
    explicit LocalExclude(QWidget *parent = nullptr);
    ~LocalExclude();

private slots:
    void onAccepted();

private:
    Ui::LocalExclude *ui;
    void loadFromConfig();
    bool saveToConfig();
};

#endif // LOCALEXCLUDE_H
