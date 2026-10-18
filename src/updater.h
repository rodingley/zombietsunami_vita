/* updater.h -- the port's auto-updater (updater.c) */
#ifndef UPDATER_H
#define UPDATER_H

/* call once per frame from the game's thread, before input is read: starts
 * the check on the first frame (while the game loads), and runs the update pop-ups */
void updater_frame(void);
/* 1 while one of the updater's pop-ups is on screen (the game gets no input
 * and the frame is shown with the system dialog drawn over it) */
int updater_busy(void);

#endif
