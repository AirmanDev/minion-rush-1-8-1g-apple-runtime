#ifndef MR_WINDOW_IOS_H
#define MR_WINDOW_IOS_H

#import <UIKit/UIKit.h>

void mr_ios_attach_scene(UIWindowScene *scene);
void mr_ios_set_scene_active(BOOL active);
void mr_ios_scene_geometry_changed(UIWindowScene *scene);

#endif
