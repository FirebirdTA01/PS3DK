/*
 * gemsample - internal header declaring the sample's own helper functions.
 * Added for GCC 14 strict-implicit-declaration; the definitions already
 * exist in source/camera.c and source/gem.c.
 */

#ifndef __GEMSAMPLE_INTERNAL_H__
#define __GEMSAMPLE_INTERNAL_H__

/* source/camera.c */
void endCamera (void);
int initCamera (void);
int readCamera (void);

/* source/gem.c */
int initGem (void);
int endGem (void);
void readGem (void);
void getImageState (void);

/* source/rsxutil.c */
int startScreen (void);
void endScreen (void);

#endif /* __GEMSAMPLE_INTERNAL_H__ */
