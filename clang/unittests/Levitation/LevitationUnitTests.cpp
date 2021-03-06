//===--- C++ Levitation LevitationUnitTests.cpp -----------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines C++ Levitation Unit Tests
//
//===----------------------------------------------------------------------===//

#define LEVITATION_ENABLE_TASK_MANAGER_LOGS

#include "clang/Levitation/Common/SimpleLogger.h"
#include "clang/Levitation/TasksManager/TasksManager.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

#include <chrono>

using namespace llvm;
using namespace clang;
using namespace levitation;

namespace {
class LevitationUnitTests : public ::testing::Test {
protected:
  void SetUp() override {
    log::Logger::createLogger(log::Level::Verbose);
  }
};

TEST_F(LevitationUnitTests, FirstTest) {

  bool Flag = false;

  {
    tasks::TasksManager TM(1);

    TM.addTask([&](tasks::TasksManager::TaskContext &Context) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      Flag = true;
      Context.Successful = true;
    });

    EXPECT_TRUE(TM.waitForTasks());
  }

  EXPECT_TRUE(Flag);
}

TEST_F(LevitationUnitTests, InnerTask) {

  bool Inside1 = false;
  bool Inside2 = false;
  bool End = false;
  tasks::TasksManager::WorkerID WID0, WID1, WID2;

  {
    tasks::TasksManager TM(2);

    auto TID0 =
    TM.addTask([&](tasks::TasksManager::TaskContext &Context) {

      WID0 = TM.getWorkerID();

      auto TID1 = TM.addTask([&] (tasks::TasksManager::TaskContext &Context) {
        Inside1 = true;
        WID1 = TM.getWorkerID();
        std::this_thread::sleep_for(std::chrono::seconds(1));
      });

      auto TID2 = TM.addTask([&] (tasks::TasksManager::TaskContext &Context) {
        Inside2 = true;
        WID2 = TM.getWorkerID();
        std::this_thread::sleep_for(std::chrono::seconds(1));
      });

      TM.waitForTasks({TID1, TID2});

      EXPECT_TRUE(Inside1);
      EXPECT_TRUE(Inside2);
      EXPECT_EQ(WID1, WID2);

      End = true;

      Context.Successful = true;
    });

    EXPECT_TRUE(TM.waitForTasks({TID0}));
    EXPECT_NE(WID0, WID1);
  }

  EXPECT_TRUE(End);
}

TEST_F(LevitationUnitTests, InnerTaskSameThread) {

  bool Inside1 = false;
  bool End = false;

  {
    tasks::TasksManager TM(1);

    auto TID0 =
    TM.addTask([&](tasks::TasksManager::TaskContext &Context) {

      auto TID1 = TM.addTask([&] (tasks::TasksManager::TaskContext &Context) {
          Inside1 = true;
          std::this_thread::sleep_for(std::chrono::seconds(1));
        },
        true /*same thread*/
      );

      TM.waitForTasks({TID1});

      EXPECT_TRUE(Inside1);

      End = true;

      Context.Successful = true;
    });

    EXPECT_TRUE(TM.waitForTasks({TID0}));
  }

  EXPECT_TRUE(End);
}

TEST_F(LevitationUnitTests, RunTask) {

  bool Inside1 = false;
  bool Inside2 = false;

  tasks::TasksManager TM(1);

  tasks::TasksManager::TaskID TID1, TID2;
  tasks::TasksManager::TaskStatus TS1, TS11, TS2;

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  TID1 = TM.runTask([&] (tasks::TasksManager::TaskContext &Context) {
      Inside1 = true;
      TID2 = TM.runTask([&] (tasks::TasksManager::TaskContext &Context) {
        Inside2 = true;
        std::this_thread::sleep_for(std::chrono::seconds(1));
      });
      TS2 = TM.getTaskStatus(TID2);
      TM.waitForTasks({TID2});
    }
  );
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  TS1 = TM.getTaskStatus(TID1);

  TM.waitForTasks({TID1, TID2});

  TS11 = TM.getTaskStatus(TID1);

  EXPECT_TRUE(Inside1);
  EXPECT_TRUE(Inside2);
  EXPECT_EQ(TS2, tasks::TasksManager::TaskStatus::Successful);
  EXPECT_EQ(TS1, tasks::TasksManager::TaskStatus::Executing);
  EXPECT_EQ(TS11, tasks::TasksManager::TaskStatus::Successful);
}

}
