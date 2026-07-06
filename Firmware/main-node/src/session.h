// =============================================================================
// session.h  —  Photo-session state machine (build plan §7).
//
// Runs on the UI task (it touches LVGL). Call sessionBegin() once, then
// sessionLoop() every UI iteration. It reads UI input flags, drives the camera
// over UART, saves to SD, and shows the result screen with the QR.
// =============================================================================
#pragma once

void sessionBegin();
void sessionLoop();
