
#ifdef __linux__

#include <xcb/xcb.h>

struct Window
{
    xcb_connection_t* conn;
};

#endif

#ifdef __APPLE__

typedef struct
{
    void* layer_ptr;
} Window;

#endif

#ifdef __cplusplus
extern "C" {
#endif

int create_window(Window* w);

int event_loop(Window* w);

#ifdef __cplusplus
};
#endif
