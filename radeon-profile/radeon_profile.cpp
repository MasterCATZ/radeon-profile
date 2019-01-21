// copyright marazmista 3.2013

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

//==================
// the logic is simple. radeon-profile create object based on gpu class.
// gpu class decides what driver is used (device.initialize()). Later, based
// on structure driverFeatures, program activate some ui features.

#include "radeon_profile.h"
#include "ui_radeon_profile.h"
#include "dialogs/dialog_defineplot.h"
#include "components/topbarcomponents.h"

#include <QTimer>
#include <QTextStream>
#include <QMenu>
#include <QDir>
#include <QDateTime>
#include <QMessageBox>
#include <QDebug>

unsigned int radeon_profile::minFanStepsSpeed = 10;


radeon_profile::radeon_profile(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::radeon_profile)
{
    ui->setupUi(this);

    // checks if running as root
    if (globalStuff::grabSystemInfo("whoami")[0] == "root") {
         globalStuff::globalConfig.rootMode = true;
        ui->label_rootWarrning->setVisible(true);
    } else {
        globalStuff::globalConfig.rootMode = false;
        ui->label_rootWarrning->setVisible(false);
    }

    ticksCounter = 0;
    statsTickCounter = 0;
	savedState = nullptr;
    timer = new QTimer(this);

    // create runtime stuff, setup rest of ui
    setupUiElements();

    loadConfig();

    if (!device.initialize()) {
        QMessageBox::critical(this,tr("Error"), tr("No Radeon cards have been found in the system."));

        for (int i = 0; i < ui->tw_main->count() - 1; ++i)
            ui->tw_main->setTabEnabled(i, false);

        return;
    }

    for (int i = 0; i < device.gpuList.count(); ++i)
        ui->combo_gpus->addItem(device.gpuList.at(i).sysName);

    // start is heavy, delegated to another thread to show ui smoothly, deleted after init
    initFuture = new QFutureWatcher<void>();
    connect(initFuture, SIGNAL(finished()), this,  SLOT(initFutureHandler()));
    initFuture->setFuture(QtConcurrent::run(this, &radeon_profile::refreshGpuData));

    // fill tables with data at the start //
    ui->list_glxinfo->addItems(device.getGLXInfo(ui->combo_gpus->currentText()));
    fillConnectors();
    fillModInfo();

    timer->start();

    showWindow();
}

radeon_profile::~radeon_profile()
{
    delete ui;
}

void radeon_profile::initFutureHandler() {
    setupUiEnabledFeatures(device.getDriverFeatures(), device.gpuData);
    ui->centralWidget->setEnabled(true);

    createPlots();

    if (topbarManager.schemas.count() == 0)
        topbarManager.createDefaultTopbarSchema(device.gpuData.keys());

    topbarManager.createTopbar(ui->topbar_layout);

    refreshUI();
    connectSignals();
    delete initFuture;
}

void radeon_profile::connectSignals()
{
    // fix for warrning: QMetaObject::connectSlotsByName: No matching signal for...
    connect(ui->combo_gpus,SIGNAL(currentIndexChanged(QString)),this,SLOT(gpuChanged()));
    connect(ui->combo_pLevel,SIGNAL(currentIndexChanged(int)),this,SLOT(setPowerLevelFromCombo()));
    connect(&group_Dpm, SIGNAL(buttonClicked(int)), this, SLOT(setPowerLevel(int)));
    connect(timer,SIGNAL(timeout()),this,SLOT(timerEvent()));
}

void radeon_profile::setupUiElements()
{
    qDebug() << "Creating ui elements";

    ui->tw_main->setCurrentIndex(0);
    ui->tw_systemInfo->setCurrentIndex(0);
    ui->list_currentGPUData->setHeaderHidden(false);
    ui->execPages->setCurrentIndex(0);
    setupForcePowerLevelMenu();
    setupContextMenus();
    setupTrayIcon();
    addRuntimeWidgets();
    createGeneralMenu();

    ui->centralWidget->setEnabled(false);
}

// based on driverFeatures structure returned by gpu class, adjust ui elements
void radeon_profile::setupUiEnabledFeatures(const DriverFeatures &features, const GPUDataContainer &data) {
    qDebug() << "Handling found device features";

    if (features.isChangeProfileAvailable && features.currentPowerMethod < PowerMethod::PM_UNKNOWN) {
        ui->stack_pm->setCurrentIndex(features.currentPowerMethod);

        changeProfile->setEnabled(features.currentPowerMethod == PowerMethod::PROFILE);
        menu_dpm->setEnabled(features.currentPowerMethod == PowerMethod::DPM);
        ui->combo_pLevel->setEnabled(features.currentPowerMethod == PowerMethod::DPM);
    } else {
        ui->stack_pm->setEnabled(false);
        changeProfile->setEnabled(false);
        menu_dpm->setEnabled(false);
        ui->combo_pLevel->setEnabled(false);
        ui->cb_eventsTracking->setEnabled(false);
        ui->cb_eventsTracking->setChecked(false);
    }

    ui->tw_systemInfo->setTabEnabled(3,data.contains(ValueID::CLK_CORE));

    if (!device.gpuData.contains(ValueID::CLK_CORE) && !data.contains(ValueID::TEMPERATURE_CURRENT) && !device.gpuData.contains(ValueID::VOLT_CORE))
        ui->tw_main->setTabEnabled(1,false);

    if (data.contains(ValueID::FAN_SPEED_PERCENT) && features.isChangeProfileAvailable) {
        qDebug() << "Fan control available";
        on_slider_fanSpeed_valueChanged(ui->slider_fanSpeed->value());
        ui->l_fanProfileUnsavedIndicator->setVisible(false);

        setupFanProfilesMenu();

        if (ui->cb_saveFanMode->isChecked()) {
            switch (ui->stack_fanModes->currentIndex()) {
                case 1:
                    on_btn_pwmFixed_clicked();
                    break;
                case 2:
                    device.getTemperature();
                    on_btn_pwmProfile_clicked();
                    break;
            }
        }
    } else {
        qDebug() << "Fan control not available";
        ui->tw_main->setTabEnabled(3,false);
        ui->btn_fanControl->setVisible(false);
        ui->group_cfgFan->setEnabled(false);
    }

    if (Q_LIKELY(features.currentPowerMethod == PowerMethod::DPM))
        ui->combo_pLevel->addItems(globalStuff::createPowerLevelCombo(device.getDriverFeatures().sysInfo.module));

    ui->group_cfgDaemon->setEnabled(device.daemonConnected());

    createCurrentGpuDataListItems();

    // oc working only on amdgpu
    if (features.isChangeProfileAvailable && device.getDriverFeatures().sysInfo.module == DriverModule::AMDGPU) {
        if (device.getDriverFeatures().isPercentCoreOcAvailable) {
            on_btn_applyOverclock_clicked();

            ui->slider_ocMclk->setEnabled(device.getDriverFeatures().isPercentMemOcAvailable);
            ui->label_memOc->setEnabled(device.getDriverFeatures().isPercentMemOcAvailable);
            ui->l_ocMclk->setEnabled(device.getDriverFeatures().isPercentMemOcAvailable);
        } else {
            ui->group_oc->setCheckable(false);
            ui->group_oc->setEnabled(false);
        }

        if (device.getDriverFeatures().isDpmCoreFreqTableAvailable) {
            ui->slider_freqSclk->setMaximum(device.getDriverFeatures().sclkTable.count() - 1);

            if (device.getDriverFeatures().isDpmMemFreqTableAvailable)
                ui->slider_freqMclk->setMaximum(device.getDriverFeatures().mclkTable.count() - 1);
            else {
                ui->slider_freqMclk->setEnabled(false);
                ui->label_memFreq->setEnabled(device.getDriverFeatures().isDpmMemFreqTableAvailable);
                ui->l_freqMclk->setEnabled(device.getDriverFeatures().isDpmMemFreqTableAvailable);
            }
        } else {
            ui->group_freq->setCheckable(false);
            ui->group_freq->setEnabled(false);
        }
    }
    else
        ui->tw_main->setTabEnabled(2, false);


    if (device.getDriverFeatures().isOcTableAvailable) {
        ui->tw_overclock->setTabEnabled(1, true);

        for (const unsigned k :  device.getDriverFeatures().coreTable.keys()) {
            const FreqVoltPair &fvt = device.getDriverFeatures().coreTable.value(k);

            series_ocClockFreq->append(k, fvt.frequency);
            series_ocCoreVolt->append(k, fvt.voltage);

            ui->list_coreStates->addTopLevelItem(new QTreeWidgetItem(QStringList() << QString::number(k)
                                                                     << QString::number(fvt.frequency)
                                                                     << QString::number(fvt.voltage)));
        }

        for (const unsigned k :  device.getDriverFeatures().memTable.keys()) {
            const FreqVoltPair &fvt = device.getDriverFeatures().memTable.value(k);

            series_ocMemFreqk->append(k, fvt.frequency);
            series_ocMemVolt->append(k, fvt.voltage);

            ui->list_memStates->addTopLevelItem(new QTreeWidgetItem(QStringList() << QString::number(k)
                                                                    << QString::number(fvt.frequency)
                                                                    << QString::number(fvt.voltage)));
        }

        axis_frequency->setRange(0, device.getDriverFeatures().coreRange.max + 100);
        axis_volts->setRange(0,  device.getDriverFeatures().voltageRange.max + 100);
        axis_state->setRange(0, device.getDriverFeatures().coreTable.lastKey());
        axis_state->setTickCount(device.getDriverFeatures().coreTable.lastKey() + 2);
        axis_frequency->setTickCount(6);
        axis_volts->setTickCount(6);
    } else {
        ui->tw_overclock->setTabEnabled(1, false);

    }
}

void radeon_profile::refreshGpuData() {
    device.refreshPowerLevel();
    device.getClocks();
    device.getTemperature();
    device.getGpuUsage();
    device.getFanSpeed();
    device.getPowerCapCurrent();
}

void radeon_profile::addTreeWidgetItem(QTreeWidget * parent, const QString &leftColumn, const QString  &rightColumn) {
    parent->addTopLevelItem(new QTreeWidgetItem(QStringList() << leftColumn << rightColumn));
}

QString radeon_profile::createCurrentMinMaxString(const ValueID idCurrent, const ValueID idMin, const ValueID idMax) {
    return QString(device.gpuData.value(idCurrent).strValue + " (min: " + device.gpuData.value(idMin).strValue + " max: " + device.gpuData.value(idMax).strValue + ")");

}

void radeon_profile::refreshUI() {
    // refresh top bar
    topbarManager.updateItems(device.gpuData);

    // GPU data list
    if (ui->tw_main->currentIndex() == 0) {
        for (int i = 0; i < ui->list_currentGPUData->topLevelItemCount(); ++i) {
            switch (keysInCurrentGpuList.value(i)) {
                case ValueID::TEMPERATURE_CURRENT:
                    ui->list_currentGPUData->topLevelItem(i)->setText(1, createCurrentMinMaxString(ValueID::TEMPERATURE_CURRENT, ValueID::TEMPERATURE_MIN, ValueID::TEMPERATURE_MAX));
                    continue;
                case ValueID::POWER_CAP_CURRENT:
                    ui->list_currentGPUData->topLevelItem(i)->setText(1, createCurrentMinMaxString(ValueID::POWER_CAP_CURRENT, ValueID::POWER_CAP_MIN, ValueID::POWER_CAP_MAX));
                    continue;

                // ignored values
                case ValueID::TEMPERATURE_BEFORE_CURRENT:
                case ValueID::TEMPERATURE_MIN:
                case ValueID::TEMPERATURE_MAX:
                case ValueID::POWER_CAP_MIN:
                case ValueID::POWER_CAP_MAX:
                    continue;

                default:
                    ui->list_currentGPUData->topLevelItem(i)->setText(1, device.gpuData.value(keysInCurrentGpuList.value(i)).strValue);
            }
        }
    }

    if (device.getDriverFeatures().currentPowerMethod == PowerMethod::DPM) {
        ui->combo_pLevel->setCurrentText(device.currentPowerLevel);
        if (device.currentPowerProfile == dpm_battery)
            ui->btn_dpmBattery->setChecked(true);
        else if (device.currentPowerProfile == dpm_balanced)
            ui->btn_dpmBalanced->setChecked(true);
        else if (device.currentPowerProfile == dpm_performance)
            ui->btn_dpmPerformance->setChecked(true);
    }

    // do the math only when user looking at stats table
    if (ui->tw_systemInfo->currentIndex() == 3 && ui->tw_main->currentIndex() == 0)
        updateStatsTable();

}

void radeon_profile::createCurrentGpuDataListItems()
{
    ui->list_currentGPUData->clear();
    for (int i = 0; i < device.gpuData.keys().count(); ++i) {

        switch (device.gpuData.keys().at(i)) {

            // ignored values
            case ValueID::TEMPERATURE_BEFORE_CURRENT:
            case ValueID::TEMPERATURE_MIN:
            case ValueID::TEMPERATURE_MAX:
            case ValueID::POWER_CAP_MIN:
            case ValueID::POWER_CAP_MAX:
                continue;

            default:
                addTreeWidgetItem(ui->list_currentGPUData, globalStuff::getNameOfValueID(device.gpuData.keys().at(i)), "");
                keysInCurrentGpuList.insert(ui->list_currentGPUData->topLevelItemCount() - 1, device.gpuData.keys().at(i));
        }
    }
}

void radeon_profile::updateExecLogs() {
    for (int i = 0; i < execsRunning.count(); i++) {
        if (execsRunning.at(i)->getExecState() == QProcess::Running && execsRunning.at(i)->logEnabled) {
            QString logData = QDateTime::currentDateTime().toString(logDateFormat) +";" + device.gpuData.value(ValueID::POWER_LEVEL, RPValue()).strValue + ";" +
                    device.gpuData.value(ValueID::CLK_CORE).strValue + ";"+
                    device.gpuData.value(ValueID::CLK_MEM).strValue + ";"+
                    device.gpuData.value(ValueID::CLK_UVD).strValue + ";"+
                    device.gpuData.value(ValueID::DCLK_UVD).strValue + ";"+
                    device.gpuData.value(ValueID::VOLT_CORE).strValue + ";"+
                    device.gpuData.value(ValueID::VOLT_MEM).strValue + ";"+
                    device.gpuData.value(ValueID::TEMPERATURE_CURRENT).strValue;
            execsRunning.at(i)->appendToLog(logData);
        }
    }
}
//===================================
// === Main timer loop  === //
void radeon_profile::timerEvent() {
    if (!refreshWhenHidden->isChecked() && this->isHidden()) {

        // even if in tray, keep the fan control active (if enabled)
        if (device.gpuData.contains(ValueID::FAN_SPEED_PERCENT) && device.getDriverFeatures().isChangeProfileAvailable && ui->btn_pwmProfile->isChecked()) {
            device.getTemperature();
            adjustFanSpeed();
        }
        return;
    }

    refreshGpuData();

    if (device.gpuData.contains(ValueID::FAN_SPEED_PERCENT) && device.getDriverFeatures().isChangeProfileAvailable && ui->btn_pwmProfile->isChecked())
        adjustFanSpeed();

    if (Q_LIKELY(ui->cb_graphs->isChecked()))
        refreshGraphs();

    if (ui->cb_eventsTracking->isChecked())
        checkEvents();

    // lets say coreClk is essential to get stats (it is disabled in ui anyway when features.clocksAvailable is false)
    if (ui->cb_stats->isChecked() && device.gpuData.contains(ValueID::CLK_CORE))
        doTheStats();

    // don't refresh ui dynamic stuff when min or hidden
    if (!isMinimized() && !isHidden())
        refreshUI();

    refreshTooltip();

    if (Q_UNLIKELY(execsRunning.count() > 0))
        updateExecLogs();
}

void radeon_profile::adjustFanSpeed() {
    if (device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value == device.gpuData.value(ValueID::TEMPERATURE_BEFORE_CURRENT).value)
        return;

    if (device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value < device.gpuData.value(ValueID::TEMPERATURE_BEFORE_CURRENT).value &&
            ui->spin_hysteresis->value() > (hysteresisRelativeTepmerature - device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value))
        return;

    hysteresisRelativeTepmerature = device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value;

    if (currentFanProfile.contains(device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value)) {
        device.setPwmValue(currentFanProfile.value(device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value));
        return;
    }

    // find bounds of current temperature
    QMap<int,unsigned int>::const_iterator high = currentFanProfile.upperBound(device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value);
    QMap<int,unsigned int>::const_iterator low = (currentFanProfile.size() > 1 ? high - 1 : high);

    int hSpeed = high.value(),
            lSpeed = low.value();

    if (high == currentFanProfile.constBegin()) {
        device.setPwmValue(hSpeed);
        return;
    }

    if (low == currentFanProfile.constEnd()) {
        device.setPwmValue(lSpeed);
        return;
    }

    // calculate two point stright line equation based on boundaries of current temperature
    // y = mx + b = (y2-y1)/(x2-x1)*(x-x1)+y1
    int hTemperature = high.key(),
            lTemperature = low.key();

    float speed = (float)(hSpeed - lSpeed) / (float)(hTemperature - lTemperature)  * (device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value - lTemperature)  + lSpeed;

    device.setPwmValue((speed < minFanStepsSpeed) ? minFanStepsSpeed : speed);

}


void radeon_profile::refreshGraphs() {
    if (ui->stack_plots->currentIndex() != 0 || plotManager.plots.count() == 0)
        return;

    plotManager.updateSeries(++ticksCounter, device.gpuData);
}

void radeon_profile::doTheStats() {
    // count ticks for stats //
    statsTickCounter++;

    // figure out pm level based on data provided
    QString pmLevelName = "Core: " + device.gpuData.value(ValueID::CLK_CORE).strValue + "  Mem: " + device.gpuData.value(ValueID::CLK_MEM).strValue;

    if (pmStats.contains(pmLevelName))
        pmStats[pmLevelName]++;
    else {
        pmStats.insert(pmLevelName, 1);
        ui->list_stats->addTopLevelItem(new QTreeWidgetItem(QStringList() << pmLevelName));
        ui->list_stats->header()->resizeSections(QHeaderView::ResizeToContents);
    }
}

void radeon_profile::updateStatsTable() {
    // do the math with percents
    for (QTreeWidgetItemIterator item(ui->list_stats); *item; item++)
        (*item)->setText(1,QString::number(pmStats.value((*item)->text(0)) * 100.0 / statsTickCounter) + "%");
}

void radeon_profile::refreshTooltip()
{
    QString tooltipdata = radeon_profile::windowTitle() + "\n" + tr("Current profile: ")+ device.currentPowerProfile + "  " + device.currentPowerLevel +"\n";

    for (short i = 0; i < ui->list_currentGPUData->topLevelItemCount(); i++)
        tooltipdata += ui->list_currentGPUData->topLevelItem(i)->text(0) + ": " + ui->list_currentGPUData->topLevelItem(i)->text(1) + '\n';

    tooltipdata.remove(tooltipdata.length() - 1, 1); //remove empty line at bootom
    icon_tray->setToolTip(tooltipdata);
}

bool radeon_profile::askConfirmation(const QString title, const QString question){
    return QMessageBox::Yes ==
            QMessageBox::question(this, title, question, QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
}

void radeon_profile::showWindow() {
    if (ui->cb_minimizeTray->isChecked() && ui->cb_startMinimized->isChecked())
        return;

    if (ui->cb_startMinimized->isChecked())
        this->showMinimized();
    else
        this->showNormal();
}
