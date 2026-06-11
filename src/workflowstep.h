/**
 * @file workflowstep.h
 * @brief 工作流步骤的元数据结构
 *
 * 该结构体仅用于描述步骤的"外观"（名称、描述、颜色），
 * 不包含任何业务逻辑，由 HuayanScheduler 和 WorkflowWidget 共同引用。
 */

#ifndef WORKFLOWSTEP_H
#define WORKFLOWSTEP_H

#include <QString>
#include <QColor>

/**
 * @brief 单个工作流步骤的元数据
 *
 * HuayanScheduler 和 WorkflowWidget 中的 kDefaultSteps
 * 均使用此结构体描述每个步骤的显示信息。
 */
struct WorkflowStep {
    QString name;   ///< 步骤名称，显示在流程图方框的主标题行（例："相机拍照"）
    QString desc;   ///< 步骤描述，显示在方框的副标题行（例："Orbbec 3D"）
    QColor  color;  ///< 步骤颜色，激活时方框背景色，非激活时自动变暗
};

#endif // WORKFLOWSTEP_H
