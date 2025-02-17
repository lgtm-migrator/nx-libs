/**************************************************************************/
/*                                                                        */
/* Copyright (c) 2001, 2011 NoMachine (http://www.nomachine.com)          */
/* Copyright (c) 2008-2014 Oleksandr Shneyder <o.shneyder@phoca-gmbh.de>  */
/* Copyright (c) 2011-2016 Mike Gabriel <mike.gabriel@das-netzwerkteam.de>*/
/* Copyright (c) 2014-2016 Mihai Moldovan <ionic@ionic.de>                */
/* Copyright (c) 2014-2016 Ulrich Sibiller <uli42@gmx.de>                 */
/* Copyright (c) 2015-2016 Qindel Group (http://www.qindel.com)           */
/*                                                                        */
/* NXAGENT, NX protocol compression and NX extensions to this software    */
/* are copyright of the aforementioned persons and companies.             */
/*                                                                        */
/* Redistribution and use of the present software is allowed according    */
/* to terms specified in the file LICENSE which comes in the source       */
/* distribution.                                                          */
/*                                                                        */
/* All rights reserved.                                                   */
/*                                                                        */
/* NOTE: This software has received contributions from various other      */
/* contributors, only the core maintainers and supporters are listed as   */
/* copyright holders. Please contact us, if you feel you should be listed */
/* as copyright holder, as well.                                          */
/*                                                                        */
/**************************************************************************/

#include "X.h"
#include "signal.h"
#include "unistd.h"
#include "Xproto.h"
#include "screenint.h"
#include "input.h"
#include "dix.h"
#include "misc.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "servermd.h"
#include "mi.h"
#include "selection.h"
#include "keysym.h"
#include "fb.h"
#include "mibstorest.h"
#include "osdep.h"

#include "Agent.h"
#include "Args.h"
#include "Atoms.h"
#include "Colormap.h"
#include "Display.h"
#include "Screen.h"
#include "Windows.h"
#include "Pixmaps.h"
#include "Keyboard.h"
#include "Keystroke.h"
#include "Events.h"
#include "Pointer.h"
#include "Rootless.h"
#include "Splash.h"
#include "Trap.h"
#include "Dialog.h"
#include "Client.h"
#include "Clipboard.h"
#include "Split.h"
#include "Drawable.h"
#include "Handlers.h"
#include "Utils.h"
#include "Error.h"

#include <nx/NX.h>
#include <nx/NXvars.h>
#include <nx/NXproto.h>

#include "xfixesproto.h"
#define Window     XlibWindow
#define Atom   XlibAtom
#define Time XlibXID
#include "X11/include/Xfixes_nxagent.h"
#undef Window
#undef Atom
#undef Time

#ifdef NXAGENT_FIXKEYS
#include "inputstr.h"
#include "input.h"
#endif

#define Time XlibXID
#include "XKBlib.h"
#undef Time

#define GC     XlibGC
#define Font   XlibFont
#define KeySym XlibKeySym
#define XID    XlibXID
#include "Xlibint.h"
#undef  GC
#undef  Font
#undef  KeySym
#undef  XID

#include <nx-X11/cursorfont.h>

#include <nx/Shadow.h>
#include "X11/include/Xrandr_nxagent.h"

#include "compext/Compext.h"

/*
 * Set here the required log level. Please note that if you want to
 * enable DEBUG here, then you need to enable DEBUG even in Rootless.c
 */

#define PANIC
#define WARNING
#undef  TEST
#undef  DEBUG

/* debug individual subsystems */
#undef DEBUG_AUTOGRAB

/* aktivate subsystems if generic DEBUG is activated */
#ifdef DEBUG
#ifndef DEBUG_AUTOGRAB
#define DEBUG_AUTOGRAB
#endif
#endif
/*
 * Log begin and end of the important handlers.
 */

#undef  BLOCKS

extern Bool nxagentOnce;

#ifdef NX_DEBUG_INPUT
int nxagentDebugInput = 0;
#endif

#ifdef DEBUG
extern Bool nxagentRootlessTreesMatch(void);
#endif

extern Selection *CurrentSelections;
extern int NumCurrentSelections;

typedef union _XFixesSelectionEvent {
        int                          type;
        XFixesSelectionNotifyEvent   xfixesselection;
        XEvent                       core;
} XFixesSelectionEvent;

Bool   xkbdRunning = False;
pid_t  pidkbd;

WindowPtr nxagentLastEnteredWindow = NULL;

PropertyRequestRec nxagentPropertyRequests[NXNumberOfResources];

void nxagentHandleCollectPropertyEvent(XEvent*);

/*
 * Finalize the asynchronous handling of the X_GrabPointer requests.
 */

void nxagentHandleCollectGrabPointerEvent(int resource);

Bool nxagentCollectGrabPointerPredicate(Display *disp, XEvent *X, XPointer ptr);

/*
 * Used in Handlers.c to synchronize the agent with the remote X
 * server.
 */

void nxagentHandleCollectInputFocusEvent(int resource);

/*
 * Viewport navigation.
 */

static int viewportInc = 1;
static enum HandleEventResult viewportLastKeyPressResult;
static int viewportLastX;
static int viewportLastY;
static Cursor viewportCursor;

#define MAX_INC 200
#define INC_STEP 5
#define nextinc(x)  ((x) < MAX_INC ? (x) += INC_STEP : (x))

/*
 * Keyboard and pointer are handled as they were real devices by Xnest
 * and we inherit this behaviour. The following mask will contain the
 * event mask selected for the root window of the agent. All the
 * keyboard and pointer events will be translated by the agent and
 * sent to the internal clients according to events selected by the
 * inferior windows.
 */

static Mask defaultEventMask;

static int lastEventSerial = 0;

/*
 * Used to mask the appropriate bits in the state reported by
 * XkbStateNotify and XkbGetIndicatorState.
 */

#define CAPSFLAG_IN_REPLY     1
#define CAPSFLAG_IN_EVENT     2
#define NUMFLAG_IN_EVENT      16
#define NUMFLAG_IN_REPLY      2

CARD32 nxagentLastEventTime     = 0;
CARD32 nxagentLastKeyPressTime  = 0;
Time   nxagentLastServerTime    = 0;

/*
 * Used for storing windows that need to receive expose events from
 * the agent.
 */

#define nxagentExposeQueueHead nxagentExposeQueue.exposures[nxagentExposeQueue.start]

ExposeQueue nxagentExposeQueue;

RegionPtr nxagentRemoteExposeRegion = NULL;

static void nxagentForwardRemoteExpose(void);

static int nxagentClipAndSendExpose(WindowPtr pWin, void * ptr);

/*
 * This is from NXproperty.c.
 */

int GetWindowProperty(WindowPtr pWin, Atom property, long longOffset,
                          long longLength, Bool delete, Atom type,
                              Atom *actualType, int *format, unsigned
                                  long *nItems, unsigned long *bytesAfter,
                                      unsigned char **propData);

/*
 * Associate a resource to a drawable and store the region affected by
 * the split operation.
 */

SplitResourceRec nxagentSplitResources[NXNumberOfResources];

/*
 * Associate a resource to an unpack operation.
 */

UnpackResourceRec nxagentUnpackResources[NXNumberOfResources];

/*
 * We have to check these before launching
 * the terminate dialog in rootless mode.
 */

Bool nxagentLastWindowDestroyed = False;
Time nxagentLastWindowDestroyedTime = 0;

/*
 * Set this flag when an user input event is received.
 */

int nxagentInputEvent = 0;

int nxagentKeyDown = 0;

void nxagentSwitchResizeMode(ScreenPtr pScreen);

int nxagentCheckWindowConfiguration(XConfigureEvent* X);

#define nxagentMonitoredDuplicate(keysym) \
    ((keysym) == XK_Left || (keysym) == XK_Up || \
        (keysym) == XK_Right || (keysym) == XK_Down || \
            (keysym) == XK_Page_Up || (keysym) == XK_Page_Down || \
                (keysym) == XK_Delete || (keysym) == XK_BackSpace)

void nxagentRemoveDuplicatedKeys(XEvent *X);

void ProcessInputEvents(void)
{
  #ifdef NX_DEBUG_INPUT
  if (nxagentDebugInput == 1)
  {
    fprintf(stderr, "%s: Processing input.\n", __func__);
  }
  #endif

  mieqProcessInputEvents();
}

#ifdef DEBUG
char * nxagentGetNotifyMode(int mode)
{
  switch (mode)
  {
    case NotifyNormal:       return "NotifyNormal";
    case NotifyGrab:         return "NotifyGrab";
    case NotifyUngrab:       return "NotifyUngrab";
    case NotifyWhileGrabbed: return "NotifyWhileGrabbed";
  }
  return "Unknown";
}
#endif

#ifdef DEBUG_TREE

/*
 * Print ID and name of window.
 */

void nxagentRemoteWindowID(Window window, Bool newline)
{
#ifdef NO_I18N
    char *winName;
#else
    XTextProperty tp;
#endif

  fprintf(stderr, "0x%x", window);

  if (!window)
  {
    fprintf(stderr, " (none) ");
  }
  else
  {
    if (window == DefaultRootWindow(nxagentDisplay))
    {
      fprintf(stderr, " (the root window) ");
    }

#ifdef NO_I18N

    if (!XFetchName(nxagentDisplay, window, &winName))
    {
      fprintf(stderr, " (has no name) ");
    }
    else if (winName)
    {
      fprintf(stderr, " \"%s\" ", winName);
      SAFE_XFree(winName);
    }

#else

    if (XGetWMName(nxagentDisplay, window, &tp) == 0)
    {
      fprintf(stderr, " (has no name) ");
    }
    else if (tp.nitems > 0)
    {
      fprintf(stderr, " \"");

      int count = 0;
      char **list = NULL;
      int ret = XmbTextPropertyToTextList(nxagentDisplay, &tp, &list, &count);

      if ((ret == Success || ret > 0) && list != NULL)
      {
        for (int i = 0; i < count; i++)
        {
          fprintf(stderr, "%s", list[i]);
        }

        XFreeStringList(list);
      }
      else
      {
        fprintf(stderr, "%s", tp.value);
      }

      fprintf(stderr, "\" ");
    }

#endif

    else
    {
      fprintf(stderr, " (has no name) ");
    }
  }

  if (newline)
  {
    fprintf(stderr, "\n");
  }

  return;
}

/*
 * Print info about remote window.
 */

void nxagentRemoteWindowInfo(Window win, int indent, Bool newLine)
{
  XWindowAttributes attributes;

  if (XGetWindowAttributes(nxagentDisplay, win, &attributes) == 0)
  {
    return;
  }

  fprintf(stderr, "%*sx=%d y=%d width=%d height=%d class=%s map_state=%s "
          "override_redirect=%s\n", indent, "", attributes.x, attributes.y,
                 attributes.width, attributes.height,
                     (attributes.class == 0) ? "CopyFromParent" :
                     ((attributes.class == 1) ? "InputOutput" : "InputOnly"),
                     (attributes.map_state == 0) ?
                         "IsUnmapped" : (attributes.map_state == 1 ?
                             "IsUnviewable" : "IsViewable"),
                                 (attributes.override_redirect == 0) ?
                                     "No" : "Yes" );

  if (newLine)
  {
    fprintf(stderr, "\n");
  }
}


/*
 * Walk remote windows tree.
 *
 * FIXME:
 * ========== nxagentRemoteWindowsTree ============
 *
 *   Root Window ID: 0x169 (the root window)  (has no name)
 *   Parent window ID: 0x2a00063 "NX Agent"
 *      0 children.
 *
 * ========== nxagentInternalWindowsTree ==========
 * Window ID=[0x9d] Remote ID=[0x2a0007e] Name: ( has no name )
 * x=0 y=0 width=1440 height=810 class=InputOutput map_state=IsViewable override_redirect=No
 *
 * -> Internal root window's remote id is not listed in RemoteWindowsTree.
 */

void nxagentRemoteWindowsTree(Window window, int level)
{
  unsigned long rootWin, parentWin;
  unsigned int numChildren;
  unsigned long *childList = NULL;

  if (!XQueryTree(nxagentDisplay, window, &rootWin, &parentWin, &childList,
                      &numChildren))
  {
    fprintf(stderr, "%s - XQueryTree failed.\n", __func__);
    return;
  }

  if (level == 0)
  {
    fprintf(stderr, "\n");

    fprintf(stderr, "  Root Window ID: ");
    nxagentRemoteWindowID(rootWin, TRUE);

    fprintf(stderr, "  Parent window ID: ");
    nxagentRemoteWindowID(parentWin, TRUE);
  }

  if (level == 0 || numChildren > 0)
  {
    fprintf(stderr, "%*s", (level * 4) + 5, ""); /* 4 spaces per level */

    fprintf(stderr, "%d child%s%s\n", numChildren, (numChildren == 1) ? "" :
               "ren", (numChildren == 1) ? ":" : ".");
  }

  for (int i = (int) numChildren - 1; i >= 0; i--)
  {
    fprintf(stderr, "%*s", (level * 5) + 6, ""); /* 5 spaces per level */

    nxagentRemoteWindowID(childList[i], TRUE);

    nxagentRemoteWindowInfo(childList[i], (level * 5) + 6, TRUE);

    nxagentRemoteWindowsTree(childList[i], level + 1);
  }

  SAFE_XFree(childList);
}

/*
 * Print info about internal window.
 */

void nxagentInternalWindowInfo(WindowPtr pWin, int indent, Bool newLine)
{
  unsigned long ulReturnItems;
  unsigned long ulReturnBytesLeft;
  Atom          atomReturnType;
  int           iReturnFormat;
  unsigned char *pszReturnData = NULL;

  fprintf(stderr, "Window ID=[0x%x] %s Remote ID=[0x%x] ", pWin -> drawable.id,
          pWin->parent ? "" : "(the root window)", nxagentWindow(pWin));

  int result = GetWindowProperty(pWin, MakeAtom("WM_NAME", 7, False) , 0,
                                     sizeof(CARD32), False, AnyPropertyType,
                                         &atomReturnType, &iReturnFormat,
                                             &ulReturnItems, &ulReturnBytesLeft,
                                                 &pszReturnData);

  fprintf(stderr, "Name: ");

  if (result == Success && pszReturnData != NULL)
  {
    fprintf(stderr, "\"%*.*s\"\n", (int)ulReturnItems, (int)ulReturnItems, (char *) pszReturnData);
  }
  else
  {
    fprintf(stderr, "%s\n", "( has no name )");
  }

  fprintf(stderr, "%*sx=%d y=%d width=%d height=%d class=%s map_state=%s "
          "override_redirect=%s", indent, "", pWin -> drawable.x, pWin -> drawable.y,
                 pWin -> drawable.width, pWin -> drawable.height,
                     (pWin -> drawable.class == 0) ? "CopyFromParent" :
                     ((pWin -> drawable.class == 1) ? "InputOutput" :
                      "InputOnly"),
                      (pWin -> mapped == 0) ?
                             "IsUnmapped" : (pWin -> realized == 0 ?
                                 "IsUnviewable" : "IsViewable"),
                                     (pWin -> overrideRedirect == 0) ?
                                         "No" : "Yes");

  if (newLine)
  {
    fprintf(stderr, "\n");
  }
}

/*
 * Walk internal windows tree.
 */

void nxagentInternalWindowsTree(WindowPtr pWin, int indent)
{
  for (; pWin; pWin = pWin -> nextSib)
  {
    fprintf(stderr, "%*s", indent, "");

    nxagentInternalWindowInfo(pWin, indent, TRUE);

    fprintf(stderr, "\n");

    nxagentInternalWindowsTree(pWin -> firstChild, indent + 4);
  }
}

#endif /* DEBUG_TREE */

void nxagentSwitchResizeMode(ScreenPtr pScreen)
{
  #ifdef DEBUG
  fprintf(stderr, "%s: Called.\n", __func__);
  #endif

  int desktopResize = nxagentOption(DesktopResize);

  nxagentChangeOption(DesktopResize, !desktopResize);

  if (!nxagentOption(DesktopResize))
  {
    fprintf(stderr,"Info: Disabled desktop resize mode in agent.\n");

    nxagentLaunchDialog(DIALOG_DISABLE_DESKTOP_RESIZE_MODE);

    if (!nxagentOption(Fullscreen))
    {
      nxagentSetWMNormalHintsMaxsize(pScreen,
                                     nxagentOption(RootWidth),
                                     nxagentOption(RootHeight));
    }
  }
  else
  {
    fprintf(stderr,"Info: Enabled desktop resize mode in agent.\n");

    nxagentLaunchDialog(DIALOG_ENABLE_DESKTOP_RESIZE_MODE);

    nxagentChangeScreenConfig(0, nxagentOption(Width), nxagentOption(Height), True);

    if (nxagentOption(ClientOs) == ClientOsWinnt)
    {
      NXSetExposeParameters(nxagentDisplay, 0, 0, 0);
    }

    nxagentSetWMNormalHintsMaxsize(pScreen,
                                   WidthOfScreen(DefaultScreenOfDisplay(nxagentDisplay)),
                                   HeightOfScreen(DefaultScreenOfDisplay(nxagentDisplay)));
  }
}

void nxagentShadowSwitchResizeMode(ScreenPtr pScreen)
{
  int desktopResize = nxagentOption(DesktopResize);

  nxagentChangeOption(DesktopResize, !desktopResize);

  if (!nxagentOption(DesktopResize))
  {
    nxagentShadowSetRatio(1.0, 1.0);

    nxagentShadowCreateMainWindow(screenInfo.screens[DefaultScreen(nxagentDisplay)], screenInfo.screens[0]->root,
                                      screenInfo.screens[0]->root -> drawable.width, screenInfo.screens[0]->root -> drawable.height);

    nxagentSetWMNormalHintsMaxsize(pScreen,
                                   nxagentOption(RootWidth),
                                   nxagentOption(RootHeight));

    fprintf(stderr,"Info: Disabled resize mode in shadow agent.\n");
  }
  else
  {
    nxagentShadowSetRatio(nxagentOption(Width) * 1.0 /
                              screenInfo.screens[0]->root -> drawable.width,
                                  nxagentOption(Height) * 1.0 /
                                      screenInfo.screens[0]->root -> drawable.height);

    nxagentShadowCreateMainWindow(screenInfo.screens[DefaultScreen(nxagentDisplay)],
                                      screenInfo.screens[0]->root, screenInfo.screens[0]->root -> drawable.width,
                                          screenInfo.screens[0]->root -> drawable.height);

    nxagentSetWMNormalHintsMaxsize(pScreen,
                                   WidthOfScreen(DefaultScreenOfDisplay(nxagentDisplay)),
                                   HeightOfScreen(DefaultScreenOfDisplay(nxagentDisplay)));

    fprintf(stderr,"Info: Enabled resize mode in shadow agent.\n");
  }
}

static void nxagentSwitchDeferMode(void)
{
  if (nxagentOption(DeferLevel) == 0)
  {
    nxagentChangeOption(DeferLevel, UNDEFINED);

    nxagentSetDeferLevel();
  }
  else
  {
    nxagentChangeOption(DeferLevel, 0);
  }

  if (nxagentOption(DeferLevel) != 0)
  {
    nxagentLaunchDialog(DIALOG_ENABLE_DEFER_MODE);
  }
  else
  {
    nxagentLaunchDialog(DIALOG_DISABLE_DEFER_MODE);

    nxagentForceSynchronization = True;
  }
}

static void nxagentEnableAutoGrab(void)
{
  #ifdef DEBUG_AUTOGRAB
  fprintf(stderr, "enabling autograb\n");
  #endif

  nxagentGrabPointerAndKeyboard(NULL);
  nxagentChangeOption(AutoGrab, True);
  nxagentLaunchDialog(DIALOG_ENABLE_AUTOGRAB_MODE);
}

static void nxagentDisableAutoGrab(void)
{
  #ifdef DEBUG_AUTOGRAB
  fprintf(stderr, "disabling autograb\n");
  #endif

  nxagentUngrabPointerAndKeyboard(NULL);
  nxagentChangeOption(AutoGrab, False);
  nxagentLaunchDialog(DIALOG_DISABLE_AUTOGRAB_MODE);
}

static void nxagentToggleAutoGrab(void)
{
  /* autograb only works in windowed mode */
  if (nxagentOption(Rootless) || nxagentOption(Fullscreen))
    return;

  if (!nxagentOption(AutoGrab))
    nxagentEnableAutoGrab();
  else
    nxagentDisableAutoGrab();
}

static Bool nxagentExposurePredicate(Display *disp, XEvent *event, XPointer window)
{
  /*
   *  Handle both Expose and ProcessedExpose events.  The latters are
   *  those not filtered by function nxagentWindowExposures().
   */

  if (window)
  {
    return ((event -> type == Expose || event -> type == ProcessedExpose) &&
                event -> xexpose.window == *((Window *) window));
  }
  else
  {
    return (event -> type == Expose || event -> type == ProcessedExpose);
  }
}

static int nxagentAnyEventPredicate(Display *disp, XEvent *event, XPointer parameter)
{
  return 1;
}

int nxagentInputEventPredicate(Display *disp, XEvent *event, XPointer parameter)
{
  switch (event -> type)
  {
    case KeyPress:
    case KeyRelease:
    case ButtonPress:
    case ButtonRelease:
    {
      return 1;
    }
    default:
    {
      return 0;
    }
  }
}

void nxagentInitDefaultEventMask(void)
{
  Mask mask = NoEventMask;

  mask |= (StructureNotifyMask | VisibilityChangeMask);

  mask |= ExposureMask;

  mask |= NXAGENT_KEYBOARD_EVENT_MASK;
  mask |= NXAGENT_POINTER_EVENT_MASK;

  defaultEventMask = mask;
}

Mask nxagentGetDefaultEventMask(void)
{
  return defaultEventMask;
}

void nxagentSetDefaultEventMask(Mask mask)
{
  defaultEventMask = mask;
}

Mask nxagentGetEventMask(WindowPtr pWin)
{
  Mask mask = NoEventMask;

  if (nxagentOption(Rootless))
  {
    /*
     * mask = pWin -> eventMask &
     *           ~(NXAGENT_KEYBOARD_EVENT_MASK | NXAGENT_POINTER_EVENT_MASK);
     */

    if (pWin -> drawable.class == InputOutput)
    {
      if (nxagentWindowTopLevel(pWin))
      {
        mask = defaultEventMask;
      }
      else
      {
        mask = ExposureMask | VisibilityChangeMask | PointerMotionMask;
      }
    }

    mask |= PropertyChangeMask;
  }
  else if (pWin -> drawable.class != InputOnly)
  {
    mask = ExposureMask | VisibilityChangeMask;
  }

  return mask;
}

static int nxagentChangeMapPrivate(WindowPtr pWin, void * ptr)
{
  if (pWin && nxagentWindowPriv(pWin))
  {
    nxagentWindowPriv(pWin) -> isMapped = *((Bool *) ptr);
  }

  return WT_WALKCHILDREN;
}

static int nxagentChangeVisibilityPrivate(WindowPtr pWin, void * ptr)
{
  if (pWin && nxagentWindowPriv(pWin))
  {
    nxagentWindowPriv(pWin) -> visibilityState = *((int *) ptr);
  }

  return WT_WALKCHILDREN;
}

void nxagentDispatchEvents(PredicateFuncPtr predicate)
{
  XEvent X;
  xEvent x;
  ScreenPtr pScreen = NULL;

  Bool minimize = False;
  Bool closeSession = False;
  Bool switchFullscreen = False;
  Bool switchAllScreens = False;

  /*
   * Last entered top level window.
   */

  static WindowPtr nxagentLastEnteredTopLevelWindow = NULL;

  #ifdef BLOCKS
  fprintf(stderr, "[Begin read]\n");
  #endif

  #ifdef TEST
  fprintf(stderr, "%s: Going to handle new events with predicate [%p].\n", __func__,
              *(void **)&predicate);
  #endif

  if (nxagentRemoteExposeRegion == NULL)
  {
    nxagentInitRemoteExposeRegion();
  }

  /*
   * We must read here, even if apparently there is nothing to
   * read. The ioctl() based readable function, in fact, is often
   * unable to detect a failure of the socket, in particular if the
   * agent was connected to the proxy and the proxy is gone. Thus we
   * must trust the wakeup handler that called us after the select().
   */

  #ifdef TEST

  if (nxagentPendingEvents(nxagentDisplay) == 0)
  {
    fprintf(stderr, "%s: PANIC! No event needs to be dispatched.\n", __func__);
  }

  #endif

  /*
   * We want to process all the events already in the queue, plus any
   * additional event that may be read from the network. If no event
   * can be read, we want to continue handling our clients without
   * flushing the output buffer.
   */

  while (nxagentCheckEvents(nxagentDisplay, &X, predicate != NULL ? predicate :
                                nxagentAnyEventPredicate, NULL) == 1)
  {
    #ifdef DEBUG
    fprintf(stderr, "%s: Going to handle new event type [%d].\n", __func__,
                X.type);
    #endif

    /*
     * Handle the incoming event.
     */

    switch (X.type)
    {
      #ifdef NXAGENT_CLIPBOARD

      case SelectionClear:
      {
        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new SelectionClear event.\n", __func__);
        #endif

        nxagentHandleSelectionClearFromXServer(&X);

        break;
      }
      case SelectionRequest:
      {
        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new SelectionRequest event.\n", __func__);
        #endif

        nxagentHandleSelectionRequestFromXServer(&X);

        break;
      }
      case SelectionNotify:
      {
        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new SelectionNotify event.\n", __func__);
        #endif

        nxagentHandleSelectionNotifyFromXServer(&X);

        break;
      }

      #endif /* NXAGENT_CLIPBOARD */

      case PropertyNotify:
      {
        #ifdef TEST
        fprintf(stderr, "%s: PropertyNotify on prop %d[%s] window %lx state %d\n", __func__,
                        (int)X.xproperty.atom, validateString(XGetAtomName(nxagentDisplay, X.xproperty.atom)),
                            X.xproperty.window, X.xproperty.state);
        #endif

        nxagentHandlePropertyNotify(&X);

        break;
      }
      case KeyPress:
      {
        enum HandleEventResult result;

        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new KeyPress event.\n", __func__);
        #endif

        nxagentInputEvent = 1;

        nxagentKeyDown++;

        nxagentHandleKeyPress(&X, &result);

        if (viewportLastKeyPressResult != result)
        {
          viewportInc = 1;

          viewportLastKeyPressResult = result;
        }

        if (result != doNothing)
        {
          pScreen = nxagentScreen(X.xkey.window);
        }

        switch (result)
        {
          case doNothing:
          {
            break;
          }

          #ifdef DEBUG_TREE

          case doDebugTree:
          {
            fprintf(stderr, "\n========== nxagentRemoteWindowsTree ============\n");
            nxagentRemoteWindowsTree(nxagentWindow(screenInfo.screens[0]->root), 0);

            fprintf(stderr, "\n========== nxagentInternalWindowsTree ==========\n");
            nxagentInternalWindowsTree(screenInfo.screens[0]->root, 0);

            break;
          }

          #endif /* DEBUG_TREE */

          case doCloseSession:
          {
            closeSession = TRUE;

            break;
          }
          case doMinimize:
          {
            minimize = TRUE;
            break;
          }
          case doSwitchFullscreen:
          {
            switchFullscreen = TRUE;
            break;
          }
          case doSwitchAllScreens:
          {
            switchAllScreens = TRUE;
            break;
          }
          case doViewportMoveUp:
          {
            nxagentMoveViewport(pScreen, 0, -nxagentOption(Height));
            break;
          }
          case doViewportMoveDown:
          {
            nxagentMoveViewport(pScreen, 0, nxagentOption(Height));
            break;
          }
          case doViewportMoveLeft:
          {
            nxagentMoveViewport(pScreen, -nxagentOption(Width), 0);
            break;
          }
          case doViewportMoveRight:
          {
            nxagentMoveViewport(pScreen, nxagentOption(Width), 0);
            break;
          }
          case doViewportUp:
          {
            nxagentMoveViewport(pScreen, 0, -nextinc(viewportInc));
            break;
          }
          case doViewportDown:
          {
            nxagentMoveViewport(pScreen, 0, +nextinc(viewportInc));
            break;
          }
          case doViewportLeft:
          {
            nxagentMoveViewport(pScreen, -nextinc(viewportInc), 0);
            break;
          }
          case doViewportRight:
          {
            nxagentMoveViewport(pScreen, +nextinc(viewportInc), 0);
            break;
          }
          case doSwitchResizeMode:
          {
            if (!nxagentOption(Shadow))
            {
              if (nxagentNoDialogIsRunning)
              {
                nxagentSwitchResizeMode(pScreen);
              }
            }
            else
            {
              nxagentShadowSwitchResizeMode(pScreen);
            }

            break;
          }
          case doSwitchDeferMode:
          {
            if (nxagentNoDialogIsRunning)
            {
              nxagentSwitchDeferMode();
            }
            break;
          }
          case doAutoGrab:
          {
            nxagentToggleAutoGrab();
            break;
          }
          case doDumpClipboard:
          {
            nxagentDumpClipboardStat();
            break;
          }
          default:
          {
            FatalError("nxagentDispatchEvent: handleKeyPress returned unknown value\n");
            break;
          }
        }

        /*
         * Elide multiple KeyPress/KeyRelease events of the same key
         * and generate a single pair. This is intended to reduce the
         * impact of the latency on the key auto-repeat, handled by
         * the remote X server. We may optionally do that only if the
         * timestamps in the events show an exces- sive delay.
         */

        KeySym keysym = XKeycodeToKeysym(nxagentDisplay, X.xkey.keycode, 0);

        if (nxagentMonitoredDuplicate(keysym) == 1)
        {
          nxagentRemoveDuplicatedKeys(&X);
        }

        if (!nxagentOption(ViewOnly) && nxagentOption(Shadow) && result == doNothing)
        {
          X.xkey.keycode = nxagentConvertKeycode(X.xkey.keycode);

          NXShadowEvent(nxagentDisplay, X);
        }

        break;
      }
      case KeyRelease:
      {
        enum HandleEventResult result;
        int sendKey = 0;

/*
FIXME: If we don't flush the queue here, it could happen that the
       inputInfo structure will not be up to date when we perform the
       following check on down keys.
*/
        ProcessInputEvents();

/*
FIXME: Don't enqueue the KeyRelease event if the key was not already
       pressed. This workaround avoids a fake KeyPress being enqueued
       by the XKEYBOARD extension.  Another solution would be to let
       the events enqueued and to remove the KeyPress afterwards.
*/
        if (BitIsOn(inputInfo.keyboard -> key -> down,
                       nxagentConvertKeycode(X.xkey.keycode)))
        {
          sendKey = 1;
        }

        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new KeyRelease event.\n", __func__);
        #endif

        nxagentInputEvent = 1;

        nxagentKeyDown--;

        if (nxagentKeyDown <= 0)
        {
          nxagentKeyDown = 0;
        }

        if (!nxagentXkbState.Initialized)
        {
          if (X.xkey.keycode == nxagentCapsLockKeycode)
          {
            nxagentXkbCapsTrap = True;
          }
          else if (X.xkey.keycode == nxagentNumLockKeycode)
          {
            nxagentXkbNumTrap = True;
          }

          nxagentInitXkbKeyboardState();

          nxagentXkbCapsTrap = False;
          nxagentXkbNumTrap = False;
        }

        /* Calculate the time elapsed between this and the last event
           we received. Add this delta to time we recorded for the
           last KeyPress event we passed on to our clients. */
        memset(&x, 0, sizeof(xEvent));
        x.u.u.type = KeyRelease;
        x.u.u.detail = nxagentConvertKeycode(X.xkey.keycode);
        x.u.keyButtonPointer.time = nxagentLastKeyPressTime +
            (X.xkey.time - nxagentLastServerTime);

        nxagentLastServerTime = X.xkey.time;

        nxagentLastEventTime = GetTimeInMillis();

        if (x.u.keyButtonPointer.time > nxagentLastEventTime)
        {
          x.u.keyButtonPointer.time = nxagentLastEventTime;
        }

        /* do not send a KeyRelease for a special keystroke since we
           also did not send a KeyPress event in that case */
        if (!(nxagentCheckSpecialKeystroke(&X.xkey, &result)) && (sendKey == 1))
        {
          #ifdef TEST
          fprintf(stderr, "%s: passing KeyRelease event to clients\n", __func__);
          #endif

          mieqEnqueue(&x);

          CriticalOutputPending = 1;

          if (!nxagentOption(ViewOnly) && nxagentOption(Shadow))
          {
            X.xkey.keycode = nxagentConvertKeycode(X.xkey.keycode);

            NXShadowEvent(nxagentDisplay, X);
          }
        }
        else
        {
          #ifdef TEST
          fprintf(stderr, "%s: NOT passing KeyRelease event to clients\n", __func__);
          #endif
        }

        break;
      }
      case ButtonPress:
      {
        #ifdef NX_DEBUG_INPUT
        if (nxagentDebugInput == 1)
        {
          fprintf(stderr, "%s: Going to handle new ButtonPress event.\n", __func__);
        }
        #endif

        nxagentInputEvent = 1;

        if (nxagentOption(Fullscreen))
        {
          if ( nxagentOption(MagicPixel) && nxagentMagicPixelZone(X.xbutton.x, X.xbutton.y) )
          {
            pScreen = nxagentScreen(X.xbutton.window);

            minimize = True;

            break;
          }
        }

        if (!nxagentOption(DesktopResize) &&
                (X.xbutton.state & (ControlMask | Mod1Mask)) == (ControlMask | Mod1Mask))
        {
          /*
           * Start viewport navigation mode.
           */

          int resource = nxagentWaitForResource(NXGetCollectGrabPointerResource,
                                                    nxagentCollectGrabPointerPredicate);

          pScreen = nxagentScreen(X.xbutton.window);
          viewportCursor = XCreateFontCursor(nxagentDisplay, XC_fleur);

          NXCollectGrabPointer(nxagentDisplay, resource,
                                   nxagentDefaultWindows[pScreen -> myNum], True,
                                       NXAGENT_POINTER_EVENT_MASK, GrabModeAsync,
                                           GrabModeAsync, None, viewportCursor,
                                               CurrentTime);
          viewportLastX = X.xbutton.x;
          viewportLastY = X.xbutton.y;

          break;
        }

        if (!(nxagentOption(Fullscreen) &&
                X.xbutton.window == nxagentFullscreenWindow &&
                    X.xbutton.subwindow == None))
        {
          memset(&x, 0, sizeof(xEvent));
          x.u.u.type = ButtonPress;
          x.u.u.detail = inputInfo.pointer -> button -> map[nxagentReversePointerMap[X.xbutton.button]];
          x.u.keyButtonPointer.time = nxagentLastEventTime = GetTimeInMillis();

          if (nxagentOption(Rootless))
          {
            x.u.keyButtonPointer.rootX = X.xmotion.x_root;
            x.u.keyButtonPointer.rootY = X.xmotion.y_root;
          }
          else
          {
            x.u.keyButtonPointer.rootX = X.xmotion.x - nxagentOption(RootX);
            x.u.keyButtonPointer.rootY = X.xmotion.y - nxagentOption(RootY);
          }

          #ifdef NX_DEBUG_INPUT
          if (nxagentDebugInput == 1)
          {
            fprintf(stderr, "%s: Adding ButtonPress event.\n", __func__);
          }
          #endif

          mieqEnqueue(&x);

          CriticalOutputPending = 1;
        }

        if (!nxagentOption(ViewOnly) && nxagentOption(Shadow))
        {
          X.xbutton.x -= nxagentOption(RootX);
          X.xbutton.y -= nxagentOption(RootY);

          if (nxagentOption(YRatio) != DONT_SCALE)
          {
            X.xbutton.x = (X.xbutton.x << PRECISION) / nxagentOption(YRatio);
          }

          if (nxagentOption(XRatio) != DONT_SCALE)
          {
            X.xbutton.y = (X.xbutton.y << PRECISION) / nxagentOption(YRatio);
          }

          NXShadowEvent(nxagentDisplay, X);
        }

        break;
      }
      case ButtonRelease:
      {
        #ifdef NX_DEBUG_INPUT
        if (nxagentDebugInput == 1)
        {
          fprintf(stderr, "%s: Going to handle new ButtonRelease event.\n", __func__);
        }
        #endif

        nxagentInputEvent = 1;

        if (viewportCursor)
        {
          /*
           * Leave viewport navigation mode.
           */

          XUngrabPointer(nxagentDisplay, CurrentTime);

          XFreeCursor(nxagentDisplay, viewportCursor);

          viewportCursor = None;
        }

        if (minimize != True)
        {
          memset(&x, 0, sizeof(xEvent));
          x.u.u.type = ButtonRelease;
          x.u.u.detail = inputInfo.pointer -> button -> map[nxagentReversePointerMap[X.xbutton.button]];
          x.u.keyButtonPointer.time = nxagentLastEventTime = GetTimeInMillis();

          if (nxagentOption(Rootless))
          {
            x.u.keyButtonPointer.rootX = X.xmotion.x_root;
            x.u.keyButtonPointer.rootY = X.xmotion.y_root;
          }
          else
          {
            x.u.keyButtonPointer.rootX = X.xmotion.x - nxagentOption(RootX);
            x.u.keyButtonPointer.rootY = X.xmotion.y - nxagentOption(RootY);
          }

          #ifdef NX_DEBUG_INPUT
          if (nxagentDebugInput == 1)
          {
            fprintf(stderr, "%s: Adding ButtonRelease event.\n", __func__);
          }
          #endif

          mieqEnqueue(&x);

          CriticalOutputPending = 1;
        }

        if (!nxagentOption(ViewOnly) && nxagentOption(Shadow))
        {
          X.xbutton.x -= nxagentOption(RootX);
          X.xbutton.y -= nxagentOption(RootY);

          if (nxagentOption(XRatio) != DONT_SCALE)
          {
            X.xbutton.x = (X.xbutton.x << PRECISION) / nxagentOption(XRatio);
          }

          if (nxagentOption(YRatio) != DONT_SCALE)
          {
            X.xbutton.y = (X.xbutton.y << PRECISION) / nxagentOption(YRatio);
          }

          NXShadowEvent(nxagentDisplay, X);
        }

        break;
      }
      case MotionNotify:
      {
        pScreen = nxagentScreen(X.xmotion.window);

        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new MotionNotify event.\n", __func__);
        #endif

        #ifdef NX_DEBUG_INPUT
        if (nxagentDebugInput == 1)
        {
          fprintf(stderr, "%s: Handling motion notify window [%ld] root [%ld] child [%ld].\n",
                      __func__, X.xmotion.window, X.xmotion.root, X.xmotion.subwindow);

          fprintf(stderr, "%s: Pointer at [%d][%d] relative root [%d][%d].\n", __func__,
		      X.xmotion.x, X.xmotion.y, X.xmotion.x_root, X.xmotion.y_root);
        }
        #endif

        memset(&x, 0, sizeof(xEvent));
        x.u.u.type = MotionNotify;

        if (nxagentOption(Rootless))
        {
          WindowPtr pWin = nxagentWindowPtr(X.xmotion.window);

          if (pWin)
          {
            nxagentLastEnteredWindow = pWin;
          }

          if (nxagentPulldownDialogPid == 0 && nxagentLastEnteredTopLevelWindow &&
                  (X.xmotion.y_root < nxagentLastEnteredTopLevelWindow -> drawable.y + 4))
          {
            if (pWin && nxagentClientIsDialog(wClient(pWin)) == 0 &&
                    nxagentLastEnteredTopLevelWindow -> parent == screenInfo.screens[0]->root &&
                        nxagentLastEnteredTopLevelWindow -> overrideRedirect == False &&
                            X.xmotion.x_root > (nxagentLastEnteredTopLevelWindow -> drawable.x +
                                (nxagentLastEnteredTopLevelWindow -> drawable.width >> 1) - 50) &&
                                    X.xmotion.x_root < (nxagentLastEnteredTopLevelWindow -> drawable.x +
                                        (nxagentLastEnteredTopLevelWindow -> drawable.width >> 1) + 50) &&
                                            nxagentOption(Menu))
            {
              nxagentPulldownDialog(nxagentLastEnteredTopLevelWindow -> drawable.id);
            }
          }

          x.u.keyButtonPointer.rootX = X.xmotion.x_root;
          x.u.keyButtonPointer.rootY = X.xmotion.y_root;
        }
        else
        {
          x.u.keyButtonPointer.rootX = X.xmotion.x - nxagentOption(RootX);
          x.u.keyButtonPointer.rootY = X.xmotion.y - nxagentOption(RootY);
        }

        x.u.keyButtonPointer.time = nxagentLastEventTime = GetTimeInMillis();

        if (viewportCursor == None &&
                !(nxagentOption(Fullscreen) &&
                    X.xmotion.window == nxagentDefaultWindows[pScreen -> myNum]
                        && X.xmotion.subwindow == None))
        {
          #ifdef NX_DEBUG_INPUT
          if (nxagentDebugInput == 1)
          {
            fprintf(stderr, "%s: Adding motion event [%d, %d] to the queue.\n", __func__,
                        x.u.keyButtonPointer.rootX, x.u.keyButtonPointer.rootY);
          }
          #endif

          mieqEnqueue(&x);
        }

        /*
         * This test is more complicated and probably not necessary, compared
         * to a simple check on viewportCursor.
         *
         * if (!nxagentOption(Fullscreen) &&
         *       (X.xmotion.state & (ControlMask | Mod1Mask | Button1Mask)) ==
         *            (ControlMask | Mod1Mask | Button1Mask))
         */

        if (viewportCursor)
        {
          /*
           * Pointer is in viewport navigation mode.
           */

          nxagentMoveViewport(pScreen, viewportLastX - X.xmotion.x, viewportLastY - X.xmotion.y);

          viewportLastX = X.xmotion.x;
          viewportLastY = X.xmotion.y;
        }

        if (!nxagentOption(ViewOnly) && nxagentOption(Shadow) && !viewportCursor)
        {
          X.xmotion.x -= nxagentOption(RootX);
          X.xmotion.y -= nxagentOption(RootY);

          if (nxagentOption(XRatio) != DONT_SCALE)
          {
            X.xmotion.x = (X.xmotion.x << PRECISION) / nxagentOption(XRatio);
          }

          if (nxagentOption(YRatio) != DONT_SCALE)
          {
            X.xmotion.y = (X.xmotion.y << PRECISION) / nxagentOption(YRatio);
          }

          NXShadowEvent(nxagentDisplay, X);
        }

        if (!nxagentOption(Shadow))
        {
          nxagentInputEvent = 1;
        }

        break;
      }
      case FocusIn:
      {
        WindowPtr pWin;

        #ifdef DEBUG
        fprintf(stderr, "%s: Going to handle new FocusIn event [0x%lx] mode: [%s]\n", __func__, X.xfocus.window, nxagentGetNotifyMode(X.xfocus.mode));
        {
          XlibWindow w;
          int revert_to;
          XGetInputFocus(nxagentDisplay, &w, &revert_to);
          fprintf(stderr, "%s: (FocusIn): Event win [0x%lx] Focus owner [0x%lx] nxagentDefaultWindows[0] [0x%x]\n", __func__, X.xfocus.window, w, nxagentDefaultWindows[0]);
        }
        #else
          #ifdef TEST
          fprintf(stderr, "%s: Going to handle new FocusIn event\n", __func__);
          #endif
        #endif

        /*
         * Here we change the focus state in the agent.  It looks like
         * this is needed only for rootless mode at present.
         */

        if (nxagentOption(Rootless) &&
                (pWin = nxagentWindowPtr(X.xfocus.window)))
        {
          SetInputFocus(serverClient, inputInfo.keyboard, pWin -> drawable.id,
                            RevertToPointerRoot, GetTimeInMillis(), False);
        }

        if (X.xfocus.detail != NotifyInferior)
        {
          pScreen = nxagentScreen(X.xfocus.window);

          if (pScreen)
          {
            nxagentDirectInstallColormaps(pScreen);
          }
        }

        if (nxagentOption(AutoGrab) && !(nxagentOption(AllScreens) || nxagentOption(Fullscreen) || nxagentOption(Rootless)))
        {
          if (X.xfocus.window == nxagentDefaultWindows[0] && X.xfocus.mode == NotifyNormal)
          {
            #if defined(DEBUG) || defined(DEBUG_AUTOGRAB)
            fprintf(stderr, "%s: (FocusIn): grabbing\n", __func__);
            #endif
            nxagentGrabPointerAndKeyboard(NULL);
          }
          /*      else
          {
            #if defined(DEBUG) || defined(DEBUG_AUTOGRAB)
            fprintf(stderr, "%s: (FocusIn): ungrabbing\n", __func__);
            #endif
            nxagentUngrabPointerAndKeyboard(NULL);
          }
          */
        }
        break;
      }
      case FocusOut:
      {
        #ifdef DEBUG
        fprintf(stderr, "%s: Going to handle new FocusOut event [0x%lx] mode: [%s]\n", __func__, X.xfocus.window, nxagentGetNotifyMode(X.xfocus.mode));
        #else
          #ifdef TEST
          fprintf(stderr, "%s: Going to handle new FocusOut event.\n", __func__);
          #endif
        #endif

        if (X.xfocus.detail != NotifyInferior)
        {
          pScreen = nxagentScreen(X.xfocus.window);

          if (pScreen)
          {
            nxagentDirectUninstallColormaps(pScreen);
          }
        }

        #ifdef NXAGENT_FIXKEYS

        {
          /*
           * Force the keys all up when focus is lost.
           */

          for (int i = 0; i < DOWN_LENGTH; i++) /* input.h */
          {
            CARD8 val = inputInfo.keyboard->key->down[i];

            if (val != 0)
            {
              for (int k = 0; k < 8; k++)
              {
                if (val & (1 << k))
                {
                  #ifdef NXAGENT_FIXKEYS_DEBUG
                  fprintf(stderr, "sending KeyRelease event for keycode: %x\n",
                              i * 8 + k);
                  #endif

                  if (!nxagentOption(Rootless) ||
                          inputInfo.keyboard->key->modifierMap[i * 8 + k])
                  {
                    memset(&x, 0, sizeof(xEvent));
                    x.u.u.type = KeyRelease;
                    x.u.u.detail = i * 8 + k;
                    x.u.keyButtonPointer.time = nxagentLastEventTime = GetTimeInMillis();

                    if (!nxagentOption(ViewOnly) && nxagentOption(Shadow))
                    {
                      XEvent xM = {0};
                      xM.type = KeyRelease;
                      xM.xkey.display = nxagentDisplay;
                      xM.xkey.type = KeyRelease;
                      xM.xkey.keycode = i * 8 + k;
                      xM.xkey.state = inputInfo.keyboard->key->state;
                      xM.xkey.time = GetTimeInMillis();
                      NXShadowEvent(nxagentDisplay, xM);
                    }

                    mieqEnqueue(&x);
                  }
                }
              }
            }
          }

          nxagentKeyDown = 0;
        }

        #endif /* NXAGENT_FIXKEYS */

        if (nxagentOption(AutoGrab) && !nxagentFullscreenWindow)
        {
          XlibWindow w;
          int revert_to;
          XGetInputFocus(nxagentDisplay, &w, &revert_to);
          if (w != nxagentDefaultWindows[0] && X.xfocus.mode == NotifyWhileGrabbed)
          {
            #if defined(DEBUG) || defined(DEBUG_AUTOGRAB)
            fprintf(stderr, "%s: (FocusOut): ungrabbing\n", __func__);
            #endif
            nxagentUngrabPointerAndKeyboard(NULL);
          }
        }
        break;
      }
      case KeymapNotify:
      {
        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new KeymapNotify event.\n", __func__);
        #endif

        break;
      }
      case EnterNotify:
      {
        WindowPtr pWin;

        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new EnterNotify event.\n", __func__);
        #endif

        if (nxagentOption(Rootless))
        {
          WindowPtr pTLWin = NULL;

          pWin = nxagentWindowPtr(X.xcrossing.window);

          if (pWin != NULL)
          {
            for (pTLWin = pWin;
                     pTLWin -> parent != pTLWin -> drawable.pScreen -> root;
                         pTLWin = pTLWin -> parent);
          }

          if (pTLWin)
          {
            nxagentLastEnteredTopLevelWindow = pTLWin;
          }

          #ifdef TEST
          fprintf(stderr, "%s: nxagentLastEnteredTopLevelWindow [%p].\n", __func__,
                      (void *)nxagentLastEnteredTopLevelWindow);
          #endif
        }

        if (nxagentOption(Rootless) && nxagentWMIsRunning &&
                (pWin = nxagentWindowPtr(X.xcrossing.window)) &&
                    nxagentWindowTopLevel(pWin) && !pWin -> overrideRedirect &&
                        (pWin -> drawable.x != X.xcrossing.x_root - X.xcrossing.x - pWin -> borderWidth ||
                            pWin -> drawable.y != X.xcrossing.y_root - X.xcrossing.y - pWin -> borderWidth))
        {
          /*
           * This code is useful for finding the window position. It
           * should be re-implemented by following the ICCCM 4.1.5
           * recommendations.
           */

          #ifdef TEST
          fprintf(stderr, "%s: pWin -> drawable.x [%d] pWin -> drawable.y [%d].\n", __func__,
                      pWin -> drawable.x, pWin -> drawable.y);
          #endif

          XID values[4];
          register XID *value = values;
          *value++ = (XID) (X.xcrossing.x_root - X.xcrossing.x - pWin -> borderWidth);
          *value++ = (XID) (X.xcrossing.y_root - X.xcrossing.y - pWin -> borderWidth);

          /*
           * nxagentWindowPriv(pWin)->x = (X.xcrossing.x_root - X.xcrossing.x);
           * nxagentWindowPriv(pWin)->y = (X.xcrossing.y_root - X.xcrossing.y);
           */

          Mask mask = CWX | CWY;

          nxagentScreenTrap = True;

          ConfigureWindow(pWin, mask, (XID *) values, wClient(pWin));

          nxagentScreenTrap = False;
        }

        if (nxagentOption(Fullscreen) &&
                X.xcrossing.window == nxagentFullscreenWindow &&
                    X.xcrossing.detail != NotifyInferior)
        {
          nxagentGrabPointerAndKeyboard(&X);
        }

        if (X.xcrossing.detail != NotifyInferior)
        {
          pScreen = nxagentScreen(X.xcrossing.window);

          if (pScreen)
          {
            NewCurrentScreen(pScreen, X.xcrossing.x, X.xcrossing.y);

            memset(&x, 0, sizeof(xEvent));
            x.u.u.type = MotionNotify;

            if (nxagentOption(Rootless))
            {
              nxagentLastEnteredWindow = nxagentWindowPtr(X.xcrossing.window);
              x.u.keyButtonPointer.rootX = X.xcrossing.x_root;
              x.u.keyButtonPointer.rootY = X.xcrossing.y_root;
            }
            else
            {
              x.u.keyButtonPointer.rootX = X.xcrossing.x - nxagentOption(RootX);
              x.u.keyButtonPointer.rootY = X.xcrossing.y - nxagentOption(RootY);
            }

            x.u.keyButtonPointer.time = nxagentLastEventTime = GetTimeInMillis();

            mieqEnqueue(&x);

            nxagentDirectInstallColormaps(pScreen);
          }
        }

        nxagentInputEvent = 1;

        break;
      }
      case LeaveNotify:
      {
        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new LeaveNotify event.\n", __func__);
        #endif

        if (nxagentOption(Rootless) && X.xcrossing.mode == NotifyNormal &&
                X.xcrossing.detail != NotifyInferior)
        {
          nxagentLastEnteredWindow = NULL;
        }

        if (!nxagentOption(AutoGrab))
        {
          if (X.xcrossing.window == nxagentDefaultWindows[0] &&
              X.xcrossing.detail != NotifyInferior &&
              X.xcrossing.mode == NotifyNormal)
          {
             nxagentUngrabPointerAndKeyboard(&X);
          }
        }

        if (X.xcrossing.detail != NotifyInferior)
        {
          pScreen = nxagentScreen(X.xcrossing.window);

          if (pScreen)
          {
            nxagentDirectUninstallColormaps(pScreen);
          }
        }

        nxagentInputEvent = 1;

        break;
      }
      case DestroyNotify:
      {
        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new DestroyNotify event.\n", __func__);
        #endif

        if (nxagentParentWindow != (Window) 0 &&
                X.xdestroywindow.window == nxagentParentWindow)
        {
          fprintf(stderr, "Warning: Unhandled destroy notify event received in agent.\n");
        }

        break;
      }
      case ClientMessage:
      {
        enum HandleEventResult result;

        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new ClientMessage event.\n", __func__);
        #endif

        nxagentHandleClientMessageEvent(&X, &result);

        if (result == doCloseSession)
        {
          closeSession = TRUE;
        }

        break;
      }
      case VisibilityNotify:
      {
        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new VisibilityNotify event.\n", __func__);
        #endif

        if (X.xvisibility.window != nxagentDefaultWindows[0])
        {
          Window window = X.xvisibility.window;

          WindowPtr pWin = nxagentWindowPtr(window);

          if (pWin && nxagentWindowPriv(pWin))
          {
            if (nxagentWindowPriv(pWin) -> visibilityState != X.xvisibility.state)
            {
              int value = X.xvisibility.state;

              if (nxagentOption(Rootless))
              {
                TraverseTree(pWin, nxagentChangeVisibilityPrivate, &value);
              }
              else
              {
                nxagentChangeVisibilityPrivate(pWin, &value);
              }
            }
          }

          #ifdef TEST
          fprintf(stderr, "%s: Suppressing visibility notify on window [%lx].\n", __func__,
                      X.xvisibility.window);
          #endif

          break;
        }

        #ifdef TEST
        fprintf(stderr, "%s: Visibility notify state is [%d] with previous [%d].\n", __func__,
                    X.xvisibility.state, nxagentVisibility);
        #endif

        nxagentVisibility = X.xvisibility.state;

        break;
      }
      case Expose:
      {
        #ifdef DEBUG
        fprintf(stderr, "%s: Going to handle new Expose event.\n", __func__);

        fprintf(stderr, "%s: WARNING! Received Expose event for drawable [%lx]"
                    " geometry [%d, %d, %d, %d] count [%d].\n", __func__,
                        X.xexpose.window, X.xexpose.x, X.xexpose.y, X.xexpose.width,
                            X.xexpose.height, X.xexpose.count);
        #endif

        nxagentHandleExposeEvent(&X);

        break;
      }
      case GraphicsExpose:
      {
        #ifdef DEBUG
        fprintf(stderr, "%s: Going to handle new GraphicsExpose event.\n", __func__);

        fprintf(stderr, "%s: WARNING! Received GraphicsExpose event "
                    "for drawable [%lx] geometry [%d, %d, %d, %d] count [%d].\n", __func__,
                        X.xgraphicsexpose.drawable, X.xgraphicsexpose.x, X.xgraphicsexpose.y,
                            X.xgraphicsexpose.width, X.xgraphicsexpose.height,
                                X.xgraphicsexpose.count);
        #endif

        nxagentHandleGraphicsExposeEvent(&X);

        break;
      }
      case NoExpose:
      {
        #ifdef DEBUG
        fprintf(stderr, "%s: Going to handle new NoExpose event.\n", __func__);
        fprintf(stderr, "%s: WARNING! Received NoExpose event for drawable [%lx].\n", __func__, X.xnoexpose.drawable);
        #endif

        break;
      }
      case CirculateNotify:
      {
        #ifdef WARNING
        fprintf(stderr, "%s: Going to handle new CirculateNotify event.\n", __func__);
        #endif

        /*
         * FIXME: Do we need this?
         *
         * WindowPtr pWin = nxagentWindowPtr(X.xcirculate.window);
         *
         * if (!pWin)
         * {
         *   pWin = nxagentRootlessTopLevelWindow(X.xcirculate.window);
         * }
         *
         * if (!pWin)
         * {
         *   break;
         * }
         *
         * XQueryTree(nxagentDisplay, DefaultRootWindow(nxagentDisplay),
         *                &root_return, &parent_return, &children_return, &nchildren_return);
         *
         * nxagentRootlessRestack(children_return, nchildren_return);
         */

        break;
      }
      case ConfigureNotify:
      {
        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new ConfigureNotify event.\n", __func__);
        #endif

        if (nxagentConfiguredSynchroWindow == X.xconfigure.window)
        {
          if (nxagentExposeQueue.exposures[nxagentExposeQueue.start].serial != X.xconfigure.x)
          {
            #ifdef WARNING
            if (nxagentVerbose)
            {
              fprintf(stderr, "%s: Requested ConfigureNotify changes didn't take place.\n", __func__);
            }
            #endif
          }

          #ifdef TEST
          fprintf(stderr, "%s: Received ConfigureNotify and going to call nxagentSynchronizeExpose.\n", __func__);
          #endif

          nxagentSynchronizeExpose();

          break;
        }

        nxagentHandleConfigureNotify(&X);

        break;
      }
      case GravityNotify:
      {
        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new GravityNotify event.\n", __func__);
        #endif

        break;
      }
      case ReparentNotify:
      {
        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new ReparentNotify event.\n", __func__);
        #endif

        nxagentHandleReparentNotify(&X);

        break;
      }
      case UnmapNotify:
      {
        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new UnmapNotify event.\n", __func__);
        #endif

        if (nxagentOption(Rootless))
        {
          WindowPtr pWin;

          if ((pWin = nxagentRootlessTopLevelWindow(X.xunmap.window)) != NULL ||
                  ((pWin = nxagentWindowPtr(X.xunmap.window)) != NULL &&
                      nxagentWindowTopLevel(pWin)))
          {
            nxagentScreenTrap = True;

            UnmapWindow(pWin, False);

            nxagentScreenTrap = False;
          }
        }

        if (nxagentUseNXTrans == 1 && !nxagentOption(Rootless) &&
                !nxagentOption(Nested) &&
                    X.xmap.window != nxagentIconWindow)
        {
          nxagentVisibility = VisibilityFullyObscured;
        }

        break;
      }
      case MapNotify:
      {
        #ifdef TEST
        fprintf(stderr, "%s: Going to handle new MapNotify event.\n", __func__);
        #endif

        if (nxagentOption(Rootless))
        {
          WindowPtr pWin;

          if ((pWin = nxagentRootlessTopLevelWindow(X.xmap.window)) != NULL ||
                  ((pWin = nxagentWindowPtr(X.xmap.window)) != NULL &&
                      nxagentWindowTopLevel(pWin)))
          {
            nxagentScreenTrap = True;

            MapWindow(pWin, wClient(pWin));

            nxagentScreenTrap = False;
          }

          if (pWin != NULL)
          {
            Bool value = 1;

            TraverseTree(pWin, nxagentChangeMapPrivate, &value);
          }
        }

        if (nxagentOption(AllScreens))
        {
          if (X.xmap.window == nxagentIconWindow)
          {
            pScreen = nxagentScreen(X.xmap.window);
            nxagentMaximizeToFullScreen(pScreen);
          }
        }

        if (nxagentOption(Fullscreen))
        {
          nxagentVisibility = VisibilityUnobscured;
        }

        /*
         * without window manager there will be no ConfigureNotify
         * event that would trigger xinerama updates. So we do that
         * once the nxagent window gets mapped.
         */
        if (!nxagentWMIsRunning &&
            X.xmap.window == nxagentDefaultWindows[nxagentScreen(X.xmap.window)->myNum])
        {
          nxagentChangeScreenConfig(nxagentScreen(X.xmap.window)->myNum, nxagentOption(Width),
                                    nxagentOption(Height), True);
        }

        break;
      }
      case MappingNotify:
      {
        XMappingEvent *mappingEvent = (XMappingEvent *) &X;

        #ifdef DEBUG
        fprintf(stderr, "%s: WARNING! Going to handle new MappingNotify event.\n", __func__);
        #endif

        if (mappingEvent -> request == MappingPointer)
        {
          nxagentInitPointerMap();
        }

        break;
      }
      default:
      {
        /*
         * Let's check if this is a XKB state modification event.
         */

        if (nxagentHandleXkbKeyboardStateEvent(&X) == 0 && nxagentHandleXFixesSelectionNotify(&X) == 0)
        {
          #ifdef TEST
          fprintf(stderr, "%s: WARNING! Unhandled event code [%d].\n", __func__, X.type);
          #endif
        }

        break;
      }

    } /* End of switch (X.type) */

    if (X.xany.serial < lastEventSerial)
    {
      /*
       * Start over.
       */

      nxagentDeleteStaticResizedWindow(0);
    }
    else
    {
      nxagentDeleteStaticResizedWindow(X.xany.serial - 1);
    }

    lastEventSerial = X.xany.serial;

  } /* End of while (...) */

  /*
   * Send the exposed regions to the clients.
   */

  nxagentForwardRemoteExpose();

  /*
   * Handle the agent window's changes.
   */

  if (closeSession)
  {
    if (nxagentOption(Persistent))
    {
      if (nxagentNoDialogIsRunning)
      {
        nxagentLaunchDialog(DIALOG_SUSPEND_SESSION);
      }
    }
    else
    {
      if (nxagentNoDialogIsRunning)
      {
        nxagentLaunchDialog(DIALOG_KILL_SESSION);
      }
    }
  }

  if (minimize)
  {
    nxagentWMDetect();

    if (nxagentWMIsRunning)
    {
      if (nxagentOption(AllScreens))
      {
        nxagentMinimizeFromFullScreen(pScreen);
      }
      else
      {
        XIconifyWindow(nxagentDisplay, nxagentDefaultWindows[0],
                           DefaultScreen(nxagentDisplay));
      }
    }
  }

  if (switchFullscreen)
  {
    if (nxagentOption(AllScreens) && nxagentOption(Fullscreen))
    {
      nxagentSwitchAllScreens(pScreen, 0);
    }
    else
    {
      nxagentSwitchFullscreen(pScreen, !nxagentOption(Fullscreen));
    }
  }

  if (switchAllScreens)
  {
    if (!nxagentOption(AllScreens) && nxagentOption(Fullscreen))
    {
      nxagentSwitchFullscreen(pScreen, False);
    }
    else
    {
      nxagentSwitchAllScreens(pScreen, !nxagentOption(AllScreens));
    }
  }

  #ifdef BLOCKS
  fprintf(stderr, "[End read]\n");
  #endif

  /*
   * Let the underlying X server code process the input events.
   */

  #ifdef BLOCKS
  fprintf(stderr, "[Begin events]\n");
  #endif

  ProcessInputEvents();

  #ifdef TEST
  fprintf(stderr, "%s: Output pending flag is [%d] critical [%d].\n", __func__,
              NewOutputPending, CriticalOutputPending);
  #endif

  /*
   * Write the events to our clients. We may flush only in the case of
   * critical output but this doesn't seem beneficial.
   *
   * if (CriticalOutputPending == 1)
   * {
   *   FlushAllOutput();
   * }
   */

  if (NewOutputPending == 1)
  {
    #ifdef TEST
    fprintf(stderr, "%s: Flushed the processed events to clients.\n", __func__);
    #endif

    FlushAllOutput();
  }

  #ifdef TEST
  if (nxagentPendingEvents(nxagentDisplay) > 0)
  {
    fprintf(stderr, "%s: WARNING! More events need to be dispatched.\n", __func__);
  }
  #endif

  #ifdef BLOCKS
  fprintf(stderr, "[End events]\n");
  #endif
}

/*
 * Functions providing the ad-hoc handling of the remote X events.
 */

int nxagentHandleKeyPress(XEvent *X, enum HandleEventResult *result)
{
  if (!nxagentXkbState.Initialized)
  {
    if (X -> xkey.keycode == nxagentCapsLockKeycode)
    {
      nxagentXkbCapsTrap = True;
    }
    else if (X -> xkey.keycode == nxagentNumLockKeycode)
    {
      nxagentXkbNumTrap = True;
    }

    nxagentInitXkbKeyboardState();

    nxagentXkbCapsTrap = False;
    nxagentXkbNumTrap = False;
  }

  if (nxagentCheckSpecialKeystroke(&X -> xkey, result))
  {
    #ifdef TEST
    fprintf(stderr, "%s: NOT passing KeyPress event to clients\n", __func__);
    #endif
    return 1;
  }

  if (X -> xkey.keycode == nxagentCapsLockKeycode)
  {
    nxagentXkbState.Caps = (~nxagentXkbState.Caps & 1);
  }
  else if (X -> xkey.keycode == nxagentNumLockKeycode)
  {
    nxagentXkbState.Num = (~nxagentXkbState.Num & 1);
  }

  nxagentLastServerTime = X -> xkey.time;

  nxagentLastEventTime = nxagentLastKeyPressTime = GetTimeInMillis();

  xEvent x = {0};
  x.u.u.type = KeyPress;
  x.u.u.detail = nxagentConvertKeycode(X -> xkey.keycode);
  x.u.keyButtonPointer.time = nxagentLastKeyPressTime;

  #ifdef TEST
  fprintf(stderr, "%s: passing KeyPress event to clients\n", __func__);
  #endif

  mieqEnqueue(&x);

  CriticalOutputPending = 1;

  return 1;
}

int nxagentHandlePropertyNotify(XEvent *X)
{
  if (nxagentOption(Rootless) && !nxagentNotifyMatchChangeProperty((XPropertyEvent *) X))
  {
    #ifdef TEST
    fprintf(stderr, "%s: Property %ld on window %lx.\n", __func__,
                X -> xproperty.atom, X -> xproperty.window);
    #endif

    if (nxagentWindowPtr(X -> xproperty.window) != NULL)
    {
      int resource = NXGetCollectPropertyResource(nxagentDisplay);

      if (resource == -1)
      {
        #ifdef WARNING
        fprintf(stderr, "%s: WARNING! Asynchronous get property queue is full.\n", __func__);
        #endif

        return 0;
      }

      NXCollectProperty(nxagentDisplay, resource,
                            X -> xproperty.window, X -> xproperty.atom, 0,
                                MAX_RETRIEVED_PROPERTY_SIZE, False, AnyPropertyType);

      nxagentPropertyRequests[resource].window = X -> xproperty.window;
      nxagentPropertyRequests[resource].property = X -> xproperty.atom;
    }
    #ifdef TEST
    else
    {
      fprintf(stderr, "%s: Failed to look up remote window property.\n", __func__);
    }
    #endif
  }

  return 1;
}

int nxagentHandleExposeEvent(XEvent *X)
{
  StaticResizedWindowStruct *resizedWinPtr = NULL;

  #ifdef DEBUG
  fprintf(stderr, "%s: Checking remote expose events.\n", __func__);
  fprintf(stderr, "%s: Looking for window id [%ld].\n", __func__, X -> xexpose.window);
  #endif

  Window window = X -> xexpose.window;

  WindowPtr pWin = nxagentWindowPtr(window);

  if (pWin != NULL)
  {
    RegionRec sum;
    RegionRec add;
    BoxRec box;

    RegionInit(&sum, (BoxRec *) NULL, 1);
/*
FIXME: This can be maybe optimized by consuming the
       events that do not match the predicate.
*/
    do
    {
      #ifdef DEBUG
      fprintf(stderr, "%s: Adding event for window id [%ld].\n", __func__, X -> xexpose.window);
      #endif

      box.x1 = pWin -> drawable.x + wBorderWidth(pWin) + X -> xexpose.x;
      box.y1 = pWin -> drawable.y + wBorderWidth(pWin) + X -> xexpose.y;

      resizedWinPtr = nxagentFindStaticResizedWindow(X -> xany.serial);

      while (resizedWinPtr)
      {
        if (resizedWinPtr -> pWin == pWin)
        {
          box.x1 += resizedWinPtr -> offX;
          box.y1 += resizedWinPtr -> offY;
        }

        resizedWinPtr = resizedWinPtr -> prev;
      }

      box.x2 = box.x1 + X -> xexpose.width;
      box.y2 = box.y1 + X -> xexpose.height;

      RegionInit(&add, &box, 1);

      RegionAppend(&sum, &add);

      RegionUninit(&add);

      if (X -> xexpose.count == 0)
      {
        break;
      }
    }
    while (nxagentCheckEvents(nxagentDisplay, X, nxagentExposurePredicate,
                                  (XPointer) &window) == 1);

    int overlap = 0;
    RegionValidate(&sum, &overlap);

    RegionIntersect(&sum, &sum,
                         &pWin->drawable.pScreen->root->winSize);

    #ifdef DEBUG
    fprintf(stderr, "%s: Sending events for window id [%ld].\n", __func__, X -> xexpose.window);
    #endif

    /*
     * If the agent has already sent auto-generated expose,
     * save received exposes for later processing.
     */

    int index = nxagentLookupByWindow(pWin);

    if (index == -1)
    {
      miWindowExposures(pWin, &sum, NullRegion);
    }
    else
    {
      RegionTranslate(&sum, -pWin -> drawable.x, -pWin -> drawable.y);

      if (nxagentExposeQueue.exposures[index].remoteRegion == NullRegion)
      {
        nxagentExposeQueue.exposures[index].remoteRegion = RegionCreate(NULL, 1);
      }

      RegionUnion(nxagentExposeQueue.exposures[index].remoteRegion,
                       nxagentExposeQueue.exposures[index].remoteRegion, &sum);

      #ifdef TEST
      fprintf(stderr, "%s: Added region for window [%u] to position [%d].\n", __func__,
                  nxagentWindow(pWin), index);
      #endif

      if (X -> xexpose.count == 0)
      {
        nxagentExposeQueue.exposures[index].remoteRegionIsCompleted = True;
      }
      else
      {
        nxagentExposeQueue.exposures[index].remoteRegionIsCompleted = False;
      }
    }

    RegionUninit(&sum);
  }

  return 1;
}

int nxagentHandleGraphicsExposeEvent(XEvent *X)
{
  /*
   * Send an expose event to client, instead of graphics expose. If
   * target drawable is a backing pixmap, send expose event for the
   * saved window, else do nothing.
   */

  StoringPixmapPtr pStoringPixmapRec = NULL;
  miBSWindowPtr pBSwindow = NULL;
  int drawableType;

  WindowPtr pWin = nxagentWindowPtr(X -> xgraphicsexpose.drawable);

  if (pWin != NULL)
  {
    drawableType = DRAWABLE_WINDOW;
  }
  else
  {
    drawableType = DRAWABLE_PIXMAP;
  }

  if (drawableType == DRAWABLE_PIXMAP)
  {
    pStoringPixmapRec = nxagentFindItemBSPixmapList(X -> xgraphicsexpose.drawable);

    if (pStoringPixmapRec == NULL)
    {
      #ifdef TEST
      fprintf(stderr, "%s: WARNING! Storing pixmap not found.\n", __func__);
      #endif

      return 1;
    }

    pBSwindow = (miBSWindowPtr) pStoringPixmapRec -> pSavedWindow -> backStorage;

    if (pBSwindow == NULL)
    {
      #ifdef TEST
      fprintf(stderr, "%s: WARNING! Back storage not found.\n", __func__);
      #endif

      return 1;
    }

    pWin = pStoringPixmapRec -> pSavedWindow;
  }

  /*
   * Rectangle affected by GraphicsExpose event.
   */

  BoxRec rect = {
    .x1 = X -> xgraphicsexpose.x,
    .y1 = X -> xgraphicsexpose.y,
    .x2 = rect.x1 + X -> xgraphicsexpose.width,
    .y2 = rect.y1 + X -> xgraphicsexpose.height,
  };

  RegionPtr exposeRegion = RegionCreate(&rect, 0);

  if (drawableType == DRAWABLE_PIXMAP)
  {
    #ifdef TEST
    fprintf(stderr, "%s: Handling GraphicsExpose event on pixmap with id [%lu].\n",
                __func__, X -> xgraphicsexpose.drawable);
    #endif

    /*
     * The exposeRegion coordinates are relative to the pixmap to
     * which GraphicsExpose event refers. But the BS coordinates of
     * the savedRegion are relative to the window.
     */

    RegionTranslate(exposeRegion, pStoringPixmapRec -> backingStoreX,
                         pStoringPixmapRec -> backingStoreY);

    /*
     * We remove from SavedRegion the part affected by the
     * GraphicsExpose event.
     */

    RegionSubtract(&(pBSwindow -> SavedRegion), &(pBSwindow -> SavedRegion),
                        exposeRegion);
  }

  /*
   * Store the exposeRegion in order to send the expose event
   * later. The coordinates must be relative to the screen.
   */

  RegionTranslate(exposeRegion, pWin -> drawable.x, pWin -> drawable.y);

  RegionUnion(nxagentRemoteExposeRegion, nxagentRemoteExposeRegion, exposeRegion);

  RegionDestroy(exposeRegion);

  return 1;
}

int nxagentHandleClientMessageEvent(XEvent *X, enum HandleEventResult *result)
{
  *result = doNothing;

  /*
   * If window is 0, message_type is 0 and format is 32 then we assume
   * event is coming from proxy.
   */

  if (X -> xclient.window == 0 &&
          X -> xclient.message_type == 0 &&
              X -> xclient.format == 32)
  {
    #ifdef TEST
    fprintf(stderr, "%s: got nxproxy event\n", __func__);
    #endif
    nxagentHandleProxyEvent(X);

    return 1;
  }

  #ifdef TEST
  char * name = XGetAtomName(nxagentDisplay, X -> xclient.message_type);
  fprintf(stderr, "nxagentHandleClientMessageEvent: ClientMessage event window [0x%lx] with "
              "message_type [%ld][%s] format [%d] type [%d] source_indication [%ld][%s] timestamp [%ld] "
                  "curwin [0x%lx].\n", X -> xclient.window, X -> xclient.message_type, name,
                      X -> xclient.format, X -> xclient.type, X -> xclient.data.l[0],
                          X -> xclient.data.l[0] == 1 ? "'application'" : X -> xclient.data.l[0] == 1 ? "'pager'" : "'none (old spec)'",
                              X -> xclient.data.l[1], X -> xclient.data.l[2]);
  SAFE_XFree(name);
  #endif

  if (nxagentOption(Rootless))
  {
    Atom message_type = nxagentRemoteToLocalAtom(X -> xclient.message_type);

    if (!ValidAtom(message_type))
    {
      #ifdef WARNING
      fprintf(stderr, "%s: WARNING Invalid type in client message.\n", __func__);
      #endif

      return 0;
    }

    WindowPtr pWin = nxagentWindowPtr(X -> xclient.window);
    if (pWin == NULL)
    {
      /*
       * If some window on the real X server sends a
       * _NET_ACTIVE_WINDOW ClientMessage to indicate the active
       * window that window will be one not belonging to nxagent so
       * this situation is perfectly legal. For all other situations
       * we print a warning.
       */
      #ifdef WARNING
      if (message_type != MakeAtom("_NET_ACTIVE_WINDOW", strlen("_NET_ACTIVE_WINDOW"), False))
      {
        fprintf(stderr, "WARNING: Invalid window in ClientMessage xclient.window [0x%lx].\n", X->xclient.window);
      }
      #endif

      return 0;
    }

    if (message_type == MakeAtom("WM_PROTOCOLS", strlen("WM_PROTOCOLS"), False))
    {
      xEvent x = {0};
      x.u.u.type = ClientMessage;
      x.u.u.detail = X -> xclient.format;
      x.u.clientMessage.window = pWin -> drawable.id;
      x.u.clientMessage.u.l.type = message_type;
      x.u.clientMessage.u.l.longs0 = nxagentRemoteToLocalAtom(X -> xclient.data.l[0]);
      x.u.clientMessage.u.l.longs1 = GetTimeInMillis();

      if (!ValidAtom(x.u.clientMessage.u.l.longs0))
      {
        #ifdef WARNING
        fprintf(stderr, "%s: WARNING Invalid value in client message of type WM_PROTOCOLS.\n", __func__);
        #endif

        return 0;
      }
      #ifdef TEST
      else
      {
        fprintf(stderr, "%s: Sent client message of type WM_PROTOCOLS and value [%s].\n", __func__,
                    validateString(NameForAtom(x.u.clientMessage.u.l.longs0)));
      }
      #endif

      TryClientEvents(wClient(pWin), &x, 1, 1, 1, 0);
    }
    else
    {
      #ifdef WARNING
      fprintf(stderr, "%s: Ignored message type %ld [%s].\n", __func__,
                  (long int) message_type, validateString(NameForAtom(message_type)));
      #endif

      return 0;
    }

    return 1;
  }

  if (X -> xclient.message_type == nxagentAtoms[1]) /* WM_PROTOCOLS */
  {
    Atom wmAtom = (Atom) X -> xclient.data.l[0];
    Atom deleteWMatom = nxagentAtoms[2]; /* WM_DELETE_WINDOW */

    if (wmAtom == deleteWMatom)
    {
      if (nxagentOnce && (nxagentClients == 0))
      {
        GiveUp(0);
      }
      else
      {
        #ifdef TEST
        fprintf(stderr, "%s: WM_DELETE_WINDOW arrived Atom = %u.\n", __func__, wmAtom);
        #endif

        if (X -> xclient.window == nxagentIconWindow)
        {
          XMapRaised(nxagentDisplay, nxagentFullscreenWindow);

          XIconifyWindow(nxagentDisplay, nxagentIconWindow,
                             DefaultScreen(nxagentDisplay));

        }

        if (X -> xclient.window == (nxagentOption(Fullscreen) ?
                nxagentIconWindow : nxagentDefaultWindows[0]) ||
                    !nxagentWMIsRunning)
        {
          *result = doCloseSession;
        }
      }
    }
  }

  return 1;
}

int nxagentHandleXkbKeyboardStateEvent(XEvent *X)
{
  XkbEvent *xkbev = (XkbEvent *) X;

  if (nxagentXkbInfo.EventBase != -1 &&
      xkbev -> type == nxagentXkbInfo.EventBase + XkbEventCode &&
          xkbev -> any.xkb_type == XkbStateNotify)
  {
    #ifdef TEST
    fprintf(stderr, "%s: Handling event with caps [%d] num [%d] locked [%d].\n", __func__,
	    nxagentXkbState.Caps, nxagentXkbState.Num, nxagentXkbState.Locked);
    #endif

    nxagentXkbState.Locked = xkbev -> state.locked_mods;

    #ifdef TEST
    fprintf(stderr, "%s: Updated XKB locked modifier bits to [%x].\n", __func__,
                nxagentXkbState.Locked);
    #endif

    nxagentXkbState.Initialized = True;

    if (nxagentXkbState.Caps == 0 &&
            (nxagentXkbState.Locked & CAPSFLAG_IN_EVENT))
    {
      nxagentXkbState.Caps = 1;

      #ifdef TEST
      fprintf(stderr, "%s: Sending fake key [66] to engage capslock.\n", __func__);
      #endif

      if (!nxagentXkbCapsTrap)
      {
        nxagentSendFakeKey(66);
      }
    }

    if (nxagentXkbState.Caps == 1 &&
          !(nxagentXkbState.Locked & CAPSFLAG_IN_EVENT))
    {
      nxagentXkbState.Caps = 0;

      #ifdef TEST
      fprintf(stderr, "%s: Sending fake key [66] to release capslock.\n", __func__);
      #endif

      nxagentSendFakeKey(66);
    }

    if (nxagentXkbState.Caps == 0 &&
          !(nxagentXkbState.Locked & CAPSFLAG_IN_EVENT) &&
              nxagentXkbCapsTrap)
    {

      #ifdef TEST
      fprintf(stderr, "%s: Sending fake key [66] to release capslock.\n", __func__);
      #endif

      nxagentSendFakeKey(66);
    }

    if (nxagentXkbState.Num == 0 &&
            (nxagentXkbState.Locked & NUMFLAG_IN_EVENT))
    {
      nxagentXkbState.Num = 1;

      #ifdef TEST
      fprintf(stderr, "%s: Sending fake key [77] to engage numlock.\n", __func__);
      #endif

      if (!nxagentXkbNumTrap)
      {
        nxagentSendFakeKey(77);
      }
    }

    if (nxagentXkbState.Num == 1 &&
            !(nxagentXkbState.Locked & NUMFLAG_IN_EVENT))
    {
      nxagentXkbState.Num = 0;

      #ifdef TEST
      fprintf(stderr, "%s: Sending fake key [77] to release numlock.\n", __func__);
      #endif

      nxagentSendFakeKey(77);
    }

    if (nxagentXkbState.Num == 0 &&
          !(nxagentXkbState.Locked & NUMFLAG_IN_EVENT) &&
              nxagentXkbNumTrap)
    {

      #ifdef TEST
      fprintf(stderr, "%s: Sending fake key [77] to release numlock.\n", __func__);
      #endif

      nxagentSendFakeKey(77);
    }

    return 1;
  }

  return 0;
}

int nxagentHandleXFixesSelectionNotify(XEvent *X)
{
  XFixesSelectionEvent *xfixesEvent = (XFixesSelectionEvent *) X;

  if (!nxagentXFixesInfo.Initialized)
  {
      #ifdef DEBUG
      fprintf(stderr, "%s: XFixes not initialized - doing nothing.\n", __func__);
      #endif
      return 0;
  }

  if (xfixesEvent -> type != (nxagentXFixesInfo.EventBase + XFixesSelectionNotify))
  {
      #ifdef DEBUG
      fprintf(stderr, "%s: event type is [%d] - doing nothing.\n", __func__, xfixesEvent->type);
      #endif
      return 0;
  }

  #ifdef DEBUG
  fprintf(stderr, "---------\n");
  #endif

  #ifdef TEST
  fprintf(stderr, "%s: Handling event.\n", __func__);
  #endif

  #ifdef DEBUG
  fprintf(stderr, "%s: Event timestamp [%ld]\n", __func__, xfixesEvent->xfixesselection.timestamp);
  fprintf(stderr, "%s: Event selection timestamp [%ld]\n", __func__, xfixesEvent->xfixesselection.selection_timestamp);
  fprintf(stderr, "%s: Event selection window [0x%lx]\n", __func__, xfixesEvent->xfixesselection.window);
  fprintf(stderr, "%s: Event selection owner [0x%lx]\n", __func__, xfixesEvent->xfixesselection.owner);
  fprintf(stderr, "%s: Event selection [%s]\n", __func__, NameForAtom(nxagentRemoteToLocalAtom(xfixesEvent->xfixesselection.selection)));

  fprintf(stderr, "%s: Subtype ", __func__);

  switch (xfixesEvent -> xfixesselection.subtype)
  {
    case SelectionSetOwner:       fprintf(stderr, "SelectionSetOwner.\n");      break;
    case SelectionWindowDestroy:  fprintf(stderr, "SelectionWindowDestroy.\n"); break;
    case SelectionClientClose:    fprintf(stderr, "SelectionClientClose.\n");   break;
    default:                      fprintf(stderr, ".\n");                       break;
  }
  #endif

  if (xfixesEvent->xfixesselection.owner && xfixesEvent->xfixesselection.owner == nxagentWindow(screenInfo.screens[0]->root))
  {
    /*
     * This is an event that must have been triggered by nxagent itself
     * - by calling XSetSelectionOwner(). As this is no news for us we
     * can ignore the event.
     */

    #ifdef DEBUG
    fprintf(stderr, "%s: (new) owner is nxagent (window is [0x%lx]) - ignoring it.\n", __func__, xfixesEvent->xfixesselection.window);
    #endif
    return 0;
  }

  /*
   * Realistically the only situation where we can receive
   * WindowDestroy or ClientClose will also end nxagent, so we do not
   * need to handle them. But the code is here, so let's keep it.
   */
  if (xfixesEvent -> xfixesselection.subtype == SelectionSetOwner||
      xfixesEvent -> xfixesselection.subtype == SelectionWindowDestroy ||
      xfixesEvent -> xfixesselection.subtype == SelectionClientClose)
  {
    /*
     * Reception of an owner change on the real X server is - for nxagent - the same as
     * receiving a SelectionClear event. We just need to tell a (possible) internal
     * owner that it is no longer owning the selection.
     */
    nxagentHandleSelectionClearFromXServerByAtom(xfixesEvent -> xfixesselection.selection);
  }
  else
  {
    #ifdef DEBUG
    fprintf(stderr, "%s: WARNING unexpected xfixesselection subtype [%d]\n", __func__, xfixesEvent -> xfixesselection.subtype);
    #endif
  }

  return 1;
}

int nxagentHandleProxyEvent(XEvent *X)
{
  switch (X -> xclient.data.l[0])
  {
    case NXNoSplitNotify:
    case NXStartSplitNotify:
    {
      /*
       * We should never receive such events in the event loop, as
       * they should be caught at the time the split is initiated.
       */

      #ifdef PANIC

      int client = (int) X -> xclient.data.l[1];

      if (X -> xclient.data.l[0] == NXNoSplitNotify)
      {
        fprintf(stderr, "%s: PANIC! NXNoSplitNotify received with client [%d].\n", __func__, client);
      }
      else
      {
        fprintf(stderr, "%s: PANIC! NXStartSplitNotify received with client [%d].\n", __func__, client);
      }

      #endif

      return 1;
    }
    case NXCommitSplitNotify:
    {
      /*
       * We need to commit an image. Image can be the result of a
       * PutSubImage() generated by Xlib, so there can be more than a
       * single image to commit, even if only one PutImage was perfor-
       * med by the agent.
       */

      int client   = (int) X -> xclient.data.l[1];
      int request  = (int) X -> xclient.data.l[2];
      int position = (int) X -> xclient.data.l[3];

      #ifdef TEST
      fprintf(stderr, "%s: NXCommitSplitNotify received with client [%d]"
                  " request [%d] and position [%d].\n", __func__,
                      client, request, position);
      #endif

      nxagentHandleCommitSplitEvent(client, request, position);

      return 1;
    }
    case NXEndSplitNotify:
    {
      /*
       * All images for the split were transferred and we need to
       * restart the client.
       */

      int client = (int) X -> xclient.data.l[1];

      #ifdef TEST
      fprintf(stderr, "%s: NXEndSplitNotify received with client [%d].\n", __func__, client);
      #endif

      nxagentHandleEndSplitEvent(client);

      return 1;
    }
    case NXEmptySplitNotify:
    {
      /*
       * All splits have been completed and none remain.
       */

      #ifdef TEST
      fprintf(stderr, "%s: NXEmptySplitNotify received.\n", __func__);
      #endif

      nxagentHandleEmptySplitEvent();

      return 1;
    }
    case NXCollectPropertyNotify:
    {
      #ifdef TEST
      int resource = (int) X -> xclient.data.l[1];

      fprintf(stderr, "%s: NXCollectPropertyNotify received with resource [%d].\n", __func__, resource);
      #endif

      nxagentHandleCollectPropertyEvent(X);

      return 1;
    }
    case NXCollectGrabPointerNotify:
    {
      int resource = (int) X -> xclient.data.l[1];

      #ifdef TEST
      fprintf(stderr, "%s: NXCollectGrabPointerNotify received with resource [%d].\n", __func__, resource);
      #endif

      nxagentHandleCollectGrabPointerEvent(resource);

      return 1;
    }
    case NXCollectInputFocusNotify:
    {
      int resource = (int) X -> xclient.data.l[1];

      /*
       * This is not used at the present moment.
       */

      #ifdef TEST
      fprintf(stderr, "%s: NXCollectInputFocusNotify received with resource [%d].\n", __func__, resource);
      #endif

      nxagentHandleCollectInputFocusEvent(resource);

      return 1;
    }
    default:
    {
      /*
       *  Not a recognized ClientMessage event.
       */

      #ifdef WARNING
      fprintf(stderr, "%s: WARNING! Not a recognized ClientMessage proxy event [%d].\n", __func__,
                  (int) X -> xclient.data.l[0]);
      #endif

      return 0;
    }
  }
}

/*
 * In this function it is assumed that we never get a configure with
 * both stacking order and geometry changed, this way we can ignore
 * stacking changes if the geometry has changed.
 */

int nxagentCheckWindowConfiguration(XConfigureEvent* X)
{
  static int x = 0;
  static int y = 0;
  static int width = 0;
  static int height = 0;
  static Window win = None;
  Bool geometryChanged = False;

  XlibWindow root_return = 0;
  XlibWindow parent_return = 0;
  XlibWindow *children_return = NULL;
  unsigned int nchildren_return = 0;
  Status result;

  if (win == X -> window)
  {
    if (x != X -> x ||
            y != X -> y ||
                width != X -> width ||
                    height != X -> height)
    {
      geometryChanged = True;
    }
  }

  win = X -> window;

  x = X -> x;
  y = X -> y;
  width = X -> width;
  height = X -> height;

  if (geometryChanged)
  {
    #ifdef TEST
    fprintf(stderr, "%s: Configure frame. No restack.\n", __func__);
    #endif

    return 1;
  }

  #ifdef TEST
  {
    fprintf(stderr, "%s: Before restacking top level window [%p]\n", __func__,
                (void *) nxagentWindowPtr(X -> window));

    for (WindowPtr pSib = screenInfo.screens[0]->root -> firstChild; pSib; pSib = pSib -> nextSib)
    {
      fprintf(stderr, "%s: Top level window: [%p].\n", __func__, (void *) pSib);
    }
  }
  #endif

  result = XQueryTree(nxagentDisplay, DefaultRootWindow(nxagentDisplay),
                          &root_return, &parent_return, &children_return, &nchildren_return);

  if (result)
  {
    nxagentRootlessRestack(children_return, nchildren_return);
  }
  else
  {
    #ifdef WARNING
    fprintf(stderr, "%s: WARNING! Failed QueryTree request.\n", __func__);
    #endif
  }

  SAFE_XFree(children_return);

  #if 0
  fprintf(stderr, "%s: Trees match: %s\n", __func__, nxagentRootlessTreesMatch() ? "Yes" : "No");
  #endif

  return 1;
}

int nxagentHandleConfigureNotify(XEvent* X)
{
  #ifdef DEBUG
  fprintf(stderr, "%s: Event info:\n", __func__);
  fprintf(stderr, "%s:   X->serial [%ld]\n", __func__, X->xconfigure.serial);
  fprintf(stderr, "%s:   X->override_redirect [%d]\n", __func__, X->xconfigure.override_redirect);
  fprintf(stderr, "%s:   X->border_width [%d]\n", __func__, X->xconfigure.border_width);
  fprintf(stderr, "%s:   X->send_event [%d]\n", __func__, X->xconfigure.send_event);
  fprintf(stderr, "%s:   X->window [0x%lx]\n", __func__, X->xconfigure.window);
  fprintf(stderr, "%s:   X->event [0x%lx]\n", __func__, X->xconfigure.event);
  fprintf(stderr, "%s:   X->x, X->y [%d][%d]\n", __func__, X->xconfigure.x, X->xconfigure.y);
  fprintf(stderr, "%s:   X->width, X->height [%d][%d]\n", __func__, X->xconfigure.width, X->xconfigure.height);
  fprintf(stderr, "%s: References:\n", __func__);
  fprintf(stderr, "%s:   DefaultWindow[0]: [0x%x]\n", __func__, nxagentDefaultWindows[0]);
  fprintf(stderr, "%s:   DefaultRootWindow(DISPLAY) [0x%lx]\n", __func__, DefaultRootWindow(nxagentDisplay));
  #endif

  if (nxagentOption(Rootless))
  {
    int sendEventAnyway = 0;

    WindowPtr pWinWindow = nxagentWindowPtr(X -> xconfigure.window);

    #ifdef TEST
    {
      WindowPtr pWinEvent  = nxagentWindowPtr(X -> xconfigure.event);

      fprintf(stderr, "%s: Generating window is [%p][%ld] target [%p][%ld].\n", __func__,
                  (void *) pWinEvent, X -> xconfigure.event, (void *) pWinWindow, X -> xconfigure.window);
    }
    #endif

    #ifdef TEST
    fprintf(stderr, "%s: New configuration for window [%p][%ld] is [%d][%d][%d][%d] send_event [%i].\n",
                __func__, (void *) pWinWindow, X -> xconfigure.window,
                    X -> xconfigure.x, X -> xconfigure.y, X -> xconfigure.width,
                        X -> xconfigure.height, X -> xconfigure.send_event);
    #endif

    WindowPtr pWin = nxagentRootlessTopLevelWindow(X -> xconfigure.window);
    if (pWin != NULL)
    {
      /*
       * Checking for new geometry or stacking order changes.
       */

      nxagentCheckWindowConfiguration((XConfigureEvent*)X);

      return 1;
    }

    if (nxagentWindowTopLevel(pWinWindow) && !X -> xconfigure.override_redirect)
    {
      XID values[5];
      register XID *value = values;

      Mask mask = CWHeight | CWWidth | CWBorderWidth;

      /* FIXME: override_redirect is always FALSE here */
      if (X -> xconfigure.send_event || !nxagentWMIsRunning ||
                X -> xconfigure.override_redirect)
      {
        *value++ = (XID)X -> xconfigure.x;
        *value++ = (XID)X -> xconfigure.y;

        /*
         * nxagentWindowPriv(pWinWindow)->x = X -> xconfigure.x;
         * nxagentWindowPriv(pWinWindow)->y = X -> xconfigure.y;
         */

        mask |= CWX | CWY;
      }

      *value++ = (XID)X -> xconfigure.width;
      *value++ = (XID)X -> xconfigure.height;
      *value++ = (XID)X -> xconfigure.border_width;

      /*
       * We don't need width and height here.
       *
       * nxagentWindowPriv(pWinWindow)->width = X -> xconfigure.width;
       * nxagentWindowPriv(pWinWindow)->height = X -> xconfigure.height;
       */

      nxagentScreenTrap = True;

      ConfigureWindow(pWinWindow, mask, (XID *) values, wClient(pWinWindow));

      nxagentScreenTrap = False;

      nxagentCheckWindowConfiguration((XConfigureEvent*)X);

      /*
       * This workaround should help with Java 1.6.0 that seems to
       * ignore non-synthetic events.
       */

      if (nxagentOption(ClientOs) == ClientOsWinnt)
      {
        #ifdef TEST
        fprintf(stderr, "%s: Apply workaround for NXWin.\n", __func__);
        #endif

        sendEventAnyway = 1;
      }

      if (sendEventAnyway || X -> xconfigure.send_event)
      {
        xEvent x = {0};

        x.u.u.type = X -> xconfigure.type | 0x80;

        x.u.configureNotify.event = pWinWindow -> drawable.id;
        x.u.configureNotify.window = pWinWindow -> drawable.id;

        if (pWinWindow -> nextSib)
        {
          x.u.configureNotify.aboveSibling = pWinWindow -> nextSib -> drawable.id;
        }
        else
        {
          x.u.configureNotify.aboveSibling = None;
        }

        x.u.configureNotify.x = X -> xconfigure.x;
        x.u.configureNotify.y = X -> xconfigure.y;
        x.u.configureNotify.width = X -> xconfigure.width;
        x.u.configureNotify.height = X -> xconfigure.height;
        x.u.configureNotify.borderWidth = X -> xconfigure.border_width;
        x.u.configureNotify.override = X -> xconfigure.override_redirect;

        TryClientEvents(wClient(pWinWindow), &x, 1, 1, 1, 0);
      }

      return 1;
    }
  }
  else /* (nxagentOption(Rootless)) */
  {
    /*
     * Save the position of the agent default window. Don't save the
     * values if the agent is in fullscreen mode.
     *
     * If we use these values to restore the position of a window
     * after that we have dynamically changed the fullscreen
     * attribute, depending on the behaviour of window manager, we
     * could be not able to place the window exactly in the requested
     * position, so let the window manager do the job for us.
     */

    ScreenPtr pScreen = nxagentScreen(X -> xconfigure.window);

    Bool doRandR = False;

    if (X -> xconfigure.window == nxagentDefaultWindows[pScreen -> myNum])
    {
      if (!nxagentOption(AllScreens))
      {
        /*
         * - WITHOUT window manager any position change is relevant
         * - WITH window manager only synthetic position changes sent
         *   by the window manager are relevant, see ICCCM Chapter 4,
         *   "Configuring the Window"
         */
        Bool updatePos = (!nxagentWMIsRunning || X -> xconfigure.send_event != 0);
        int newX = X -> xconfigure.x;
        int newY = X -> xconfigure.y;

        if (nxagentOption(DesktopResize))
        {
          if (nxagentOption(Width) != X -> xconfigure.width ||
                nxagentOption(Height) != X -> xconfigure.height ||
                  (updatePos && (nxagentOption(X) != newX ||
                                   nxagentOption(Y) != newY)))
          {
            #ifdef DEBUG
            int count = 0;
            #endif
            Bool newEvents = False;

            doRandR = True;

            NXFlushDisplay(nxagentDisplay, NXFlushLink);

            do
            {
              newEvents = False;

              nxagentWaitEvents(nxagentDisplay, 500);

              /*
               * This should also flush the NX link for us.
               */

              XSync(nxagentDisplay, 0);

              while (XCheckTypedWindowEvent(nxagentDisplay, nxagentDefaultWindows[pScreen -> myNum],
                                              ConfigureNotify, X))
              {
                #ifdef DEBUG
                count++;
                #endif

                if (!nxagentWMIsRunning || X -> xconfigure.send_event)
                {
                  updatePos = True;
                  #ifdef DEBUG
                  fprintf(stderr, "%s: Accumulating event %d: x [%d] y [%d] width [%d] height [%d]\n", __func__, count,
                          X -> xconfigure.x, X -> xconfigure.y, X -> xconfigure.width, X -> xconfigure.height);
                  #endif
                  newX = X -> xconfigure.x;
                  newY = X -> xconfigure.y;
                }
                newEvents = True;
              }

            } while (newEvents);

            #ifdef DEBUG
            fprintf(stderr, "%s: accumulated %d events\n", __func__, count);
            #endif
          }
        }

        if (updatePos && (nxagentOption(X) != newX || nxagentOption(Y) != newY))
        {
          #ifdef DEBUG
          fprintf(stderr, "%s: Updating nxagent window position [%d,%d] -> [%d,%d]\n", __func__,
                      nxagentOption(X), nxagentOption(Y), newX, newY);
          #endif
          nxagentChangeOption(X, newX);
          nxagentChangeOption(Y, newY);
        }

        if (nxagentOption(Shadow) && nxagentOption(DesktopResize) &&
                (nxagentOption(Width) != X -> xconfigure.width ||
                    nxagentOption(Height) != X -> xconfigure.height))
        {
          nxagentShadowResize = True;
        }

        if (nxagentOption(Width) != X->xconfigure.width || nxagentOption(Height) != X->xconfigure.height)
        {
          #ifdef DEBUG
          fprintf(stderr, "%s: Updating width and height [%d,%d] -> [%d,%d]\n", __func__,
                      nxagentOption(Width), nxagentOption(Height),
                          X->xconfigure.width, X->xconfigure.height);
          #endif
          nxagentChangeOption(Width, X -> xconfigure.width);
          nxagentChangeOption(Height, X -> xconfigure.height);
        }

        nxagentChangeOption(ViewportXSpan, (int) X -> xconfigure.width -
                                (int) nxagentOption(RootWidth));
        nxagentChangeOption(ViewportYSpan, (int) X -> xconfigure.height -
                                (int) nxagentOption(RootHeight));

        nxagentMoveViewport(pScreen, 0, 0);

        /* if in shadowing mode or if neither size nor position have
           changed we do not need to adjust RandR */
        /* FIXME: Comment makes no sense */
        if (nxagentOption(Shadow) ||
                (nxagentOption(Width) == nxagentOption(RootWidth) &&
                 nxagentOption(Height) == nxagentOption(RootHeight) &&
                 nxagentOption(X) == nxagentOption(RootX) &&
                 nxagentOption(Y) == nxagentOption(RootY)))
        {
          doRandR = False;
        }

        XMoveResizeWindow(nxagentDisplay, nxagentInputWindows[0], 0, 0,
                              X -> xconfigure.width, X -> xconfigure.height);

        if (!nxagentOption(Fullscreen))
        {
	  /* FIXME: has already been done some lines above */
          nxagentMoveViewport(pScreen, 0, 0);
        }
        else
        {
          nxagentChangeOption(RootX, (nxagentOption(Width) -
                                  nxagentOption(RootWidth)) / 2);
          nxagentChangeOption(RootY, (nxagentOption(Height) -
                                  nxagentOption(RootHeight)) / 2);
          nxagentChangeOption(ViewportXSpan, nxagentOption(Width) -
                                  nxagentOption(RootWidth));
          nxagentChangeOption(ViewportYSpan, nxagentOption(Height) -
                                  nxagentOption(RootHeight));

          nxagentUpdateViewportFrame(0, 0, nxagentOption(RootWidth),
                                         nxagentOption(RootHeight));

          XMoveWindow(nxagentDisplay, nxagentWindow(pScreen->root),
                          nxagentOption(RootX), nxagentOption(RootY));
        }

        if (doRandR)
        {
          #ifdef TEST
          fprintf(stderr,"%s: Width %d Height %d.\n", __func__,
                      nxagentOption(Width), nxagentOption(Height));
          #endif
          /*
           * we are processing a ConfigureNotifyEvent that brought us
           * the current window size. If we issue a XResizeWindow()
           * again with these values we might end up in loop if the
           * window manager adjusts the size, which is perfectly
           * legal for it to do. So we prevent the XResizeWindow call
           * from happening.
           */
          nxagentChangeScreenConfig(0, nxagentOption(Width),
                                       nxagentOption(Height), False);
        }
      }

      return 1;
    }
    else
    {
      if ( (X -> xconfigure.window == DefaultRootWindow(nxagentDisplay)) || nxagentFullscreenWindow )
      {
        #ifdef TEST
        fprintf(stderr, "%s: remote root window has changed: %d,%d %dx%d\n", __func__,
                    X -> xconfigure.x, X -> xconfigure.y, X -> xconfigure.width, X -> xconfigure.height);
        #endif

        nxagentChangeOption(RootX, X -> xconfigure.x);
        nxagentChangeOption(RootY, X -> xconfigure.y);
        nxagentChangeOption(RootWidth, X -> xconfigure.width);
        nxagentChangeOption(RootHeight, X -> xconfigure.height);

        nxagentChangeScreenConfig(0, nxagentOption(Width),
                                     nxagentOption(Height), True);

        return 1;
      }
    }
  }

  #ifdef TEST
  fprintf(stderr, "%s: received for unexpected window [%ld]\n", __func__, X -> xconfigure.window);
  #endif

  return 0;
}

int nxagentHandleReparentNotify(XEvent* X)
{
  #ifdef TEST
  fprintf(stderr, "%s: Going to handle a new reparent event (serial [%ld].\n", __func__, X->xreparent.serial);
  #endif

  #ifdef DEBUG
  fprintf(stderr, "%s: Event info:\n", __func__);
  fprintf(stderr, "%s:   X->send_event [%d]\n", __func__, X->xreparent.send_event);
  fprintf(stderr, "%s:   X->event [0x%lx]\n", __func__, X->xreparent.event);
  fprintf(stderr, "%s:   X->window [0x%lx]\n", __func__, X->xreparent.window);
  fprintf(stderr, "%s:   X->parent [0x%lx]\n", __func__, X->xreparent.parent);
  fprintf(stderr, "%s:   X->x, X->y [%d][%d]\n", __func__, X->xreparent.x, X->xreparent.y);
  fprintf(stderr, "%s:   X->override_redirect [%d]\n", __func__, X->xreparent.override_redirect);
  fprintf(stderr, "%s: References:\n", __func__);
  fprintf(stderr, "%s:   DefaultWindow[0]: [0x%x]\n", __func__, nxagentDefaultWindows[0]);
  fprintf(stderr, "%s:   RootWindow(DISPLAY, 0): [0x%lx]\n", __func__, RootWindow(nxagentDisplay, 0));
  fprintf(stderr, "%s:   DefaultRootWindow(DISPLAY): [0x%lx]\n", __func__, DefaultRootWindow(nxagentDisplay));
  #endif

  if (nxagentOption(Rootless))
  {
    WindowPtr pWin = nxagentWindowPtr(X -> xreparent.window);

    #ifdef TEST

    {
      WindowPtr pParent = nxagentWindowPtr(X -> xreparent.parent);
      WindowPtr pEvent = nxagentWindowPtr(X -> xreparent.event);

      fprintf(stderr, "%s: event %p[%lx] window %p[%lx] parent %p[%lx] at (%d, %d)\n", __func__,
                  (void*)pEvent, X -> xreparent.event, (void*)pWin, X -> xreparent.window,
                          (void*)pParent, X -> xreparent.parent, X -> xreparent.x, X -> xreparent.y);
    }

    #endif

    if (nxagentWindowTopLevel(pWin))
    {
      /*
       * If the window manager reparents our top level window, we need
       * to know the new top level ancestor.
       */

      XlibWindow w = None;
      XlibWindow root_return = 0;
      XlibWindow *children_return = NULL;
      unsigned int nchildren_return = 0;
      Status result;

      XlibWindow parent_return = X -> xreparent.parent;

      while (parent_return != RootWindow(nxagentDisplay, 0))
      {
        w = parent_return;
        result = XQueryTree(nxagentDisplay, w, &root_return,
                                &parent_return, &children_return, &nchildren_return);

        SAFE_XFree(children_return);

        if (!result)
        {
          #ifdef WARNING
          fprintf(stderr, "%s: WARNING! Failed QueryTree request.\n", __func__);
          #endif

          break;
        }
      }

      if (w && !nxagentWindowPtr(w))
      {
        XSelectInput(nxagentDisplay, w, StructureNotifyMask);

        nxagentRootlessAddTopLevelWindow(pWin, w);

        #ifdef TEST
        fprintf(stderr, "%s: new top level window [%ld].\n", __func__, w);
        fprintf(stderr, "%s: reparented window [%ld].\n", __func__, X -> xreparent.window);
        #endif

        result = XQueryTree(nxagentDisplay, DefaultRootWindow(nxagentDisplay),
                                &root_return, &parent_return, &children_return, &nchildren_return);

        if (result)
        {
          nxagentRootlessRestack(children_return, nchildren_return);
        }
        else
        {
          #ifdef WARNING
          fprintf(stderr, "%s: WARNING! Failed QueryTree request.\n", __func__);
          #endif
        }

        SAFE_XFree(children_return);
      }
      else
      {
        #ifdef TEST
        fprintf(stderr, "%s: Window at [%p] has been reparented to [%ld] top level parent [%ld].\n",
                    __func__, (void *) pWin, X -> xreparent.parent, w);
        #endif

        nxagentRootlessDelTopLevelWindow(pWin);
      }
    }

    return 1;
  }
  else if (nxagentWMIsRunning && !nxagentOption(Fullscreen) &&
               nxagentOption(WMBorderWidth) == -1)
  {
    /*
     * Calculate the absolute upper-left X e Y
     */

    XlibWindow parent = X -> xreparent.parent;
    XWindowAttributes attributes;
    if ((XGetWindowAttributes(nxagentDisplay, parent, &attributes) == 0))
    {
      #ifdef WARNING
      fprintf(stderr, "%s: WARNING! XGetWindowAttributes for parent window failed.\n", __func__);
      #endif

      return 1;
    }

    XlibWindow junk;
    int x = attributes.x;
    int y = attributes.y;

    #ifdef DEBUG
    int before_x = x;
    int before_y = y;
    #endif

    XTranslateCoordinates(nxagentDisplay, parent,
                              attributes.root, -attributes.border_width,
                                  -attributes.border_width, &x, &y, &junk);

    #ifdef DEBUG
    fprintf(stderr, "%s: translated coordinates x,y [%d,%d] -> [%d,%d].\n", __func__, before_x, before_y, x, y);
    #endif

    /*
    * Calculate the parent X and parent Y.
    */

    if (parent != DefaultRootWindow(nxagentDisplay))
    {
      XlibWindow rootReturn = 0;
      XlibWindow parentReturn = 0;
      XlibWindow *childrenReturn = NULL;
      unsigned int nchildrenReturn = 0;

      do
      {
        Status result = XQueryTree(nxagentDisplay, parent, &rootReturn, &parentReturn,
                                       &childrenReturn, &nchildrenReturn);

        SAFE_XFree(childrenReturn);

        if (parentReturn == rootReturn || parentReturn == 0 || result == 0)
        {
          break;
        }

        parent = parentReturn;
      }
      while (True);

      /*
       * WM reparented. Find edge of the frame.
       */

      if (XGetWindowAttributes(nxagentDisplay, parent, &attributes) == 0)
      {
        #ifdef WARNING
        fprintf(stderr, "%s: WARNING! XGetWindowAttributes failed for parent window.\n", __func__);
        #endif

        return 1;
      }

      /*
       * Difference between Absolute X and Parent X gives thickness of side frame.
       * Difference between Absolute Y and Parent Y gives thickness of title bar.
       */

      nxagentChangeOption(WMBorderWidth, (x - attributes.x));
      nxagentChangeOption(WMTitleHeight, (y - attributes.y));

      #ifdef DEBUG
      fprintf(stderr, "%s: WMBorderWidth [%d]\n", __func__, nxagentOption(WMBorderWidth));
      fprintf(stderr, "%s: WMTitleHeight [%d]\n", __func__, nxagentOption(WMTitleHeight));
      fprintf(stderr, "%s: win_gravity [%d]\n", __func__, attributes.win_gravity);
      fprintf(stderr, "%s: bit_gravity [%d]\n", __func__, attributes.bit_gravity);
      fprintf(stderr, "%s: border_width [%d]\n", __func__, attributes.border_width);
      fprintf(stderr, "%s: height [%d]\n", __func__, attributes.height);
      fprintf(stderr, "%s: width [%d]\n", __func__, attributes.width);
      fprintf(stderr, "%s: x [%d]\n", __func__, attributes.x);
      fprintf(stderr, "%s: y [%d]\n", __func__, attributes.y);
      #endif
    }
  }

  return 1;
}

/*
 * Helper for nxagent(Enable|Disable)(Keyboard|Pointer)Events
 */
static void nxagentSwitchEventsAllScreens(Mask mask, Bool enable)
{
  Mask newmask = nxagentGetDefaultEventMask();

  if (enable)
    newmask |= mask;
  else
    newmask &= ~mask;

  nxagentSetDefaultEventMask(newmask);

  for (int i = 0; i < nxagentNumScreens; i++)
  {
    XSelectInput(nxagentDisplay, nxagentDefaultWindows[i], newmask);
  }
}

void nxagentEnableKeyboardEvents(void)
{
  nxagentSwitchEventsAllScreens(NXAGENT_KEYBOARD_EVENT_MASK, True);

  XkbSelectEvents(nxagentDisplay, XkbUseCoreKbd,
                      NXAGENT_KEYBOARD_EXTENSION_EVENT_MASK,
                          NXAGENT_KEYBOARD_EXTENSION_EVENT_MASK);
}

void nxagentDisableKeyboardEvents(void)
{
  nxagentSwitchEventsAllScreens(NXAGENT_KEYBOARD_EVENT_MASK, False);

  XkbSelectEvents(nxagentDisplay, XkbUseCoreKbd, 0x0, 0x0);
}

void nxagentEnablePointerEvents(void)
{
  nxagentSwitchEventsAllScreens(NXAGENT_POINTER_EVENT_MASK, True);
}

void nxagentDisablePointerEvents(void)
{
  nxagentSwitchEventsAllScreens(NXAGENT_POINTER_EVENT_MASK, False);
}

void nxagentSendFakeKey(int key)
{
  Time now = GetTimeInMillis();

  xEvent fake = {0};
  fake.u.u.type = KeyPress;
  fake.u.u.detail = key;
  fake.u.keyButtonPointer.time = now;

  mieqEnqueue(&fake);

  fake.u.u.type = KeyRelease;
  fake.u.u.detail = key;
  fake.u.keyButtonPointer.time = now;

  mieqEnqueue(&fake);
}

int nxagentInitXkbKeyboardState(void)
{
  XEvent X = {0};

  XkbEvent *xkbev = (XkbEvent *) &X;

  if (nxagentXkbInfo.EventBase == -1)
  {
      return 1;
  }

  #ifdef TEST
  fprintf(stderr, "%s: Initializing XKB state.\n", __func__);
  #endif

  unsigned int modifiers;
  XkbGetIndicatorState(nxagentDisplay, XkbUseCoreKbd, &modifiers);

  xkbev -> state.locked_mods = 0x0;

  if (modifiers & CAPSFLAG_IN_REPLY)
  {
    xkbev -> state.locked_mods |= CAPSFLAG_IN_EVENT;
  }

  if (modifiers & NUMFLAG_IN_REPLY)
  {
    xkbev -> state.locked_mods |= NUMFLAG_IN_EVENT;
  }

  #ifdef TEST
  fprintf(stderr, "%s: Assuming XKB locked modifier bits [%x].\n", __func__,
              xkbev -> state.locked_mods);
  #endif

  xkbev -> type         = nxagentXkbInfo.EventBase + XkbEventCode;
  xkbev -> any.xkb_type = XkbStateNotify;

  nxagentHandleXkbKeyboardStateEvent(&X);

  return 1;
}

int nxagentWaitForResource(GetResourceFuncPtr pGetResource, PredicateFuncPtr pPredicate)
{
  int resource;

  while ((resource = (*pGetResource)(nxagentDisplay)) == -1)
  {
    if (nxagentWaitEvents(nxagentDisplay, 0) == -1)
    {
      return -1;
    }

    nxagentDispatchEvents(pPredicate);
  }

  return resource;
}

void nxagentGrabPointerAndKeyboard(XEvent *X)
{
  #ifdef TEST
  fprintf(stderr, "%s: Grabbing pointer and keyboard with event at [%p].\n", __func__,
              (void *) X);
  #endif

  unsigned long now;

  if (X != NULL)
  {
    now = X -> xcrossing.time;
  }
  else
  {
    now = CurrentTime;
  }

  #ifdef TEST
  fprintf(stderr, "%s: Going to grab the keyboard in context [B1].\n", __func__);
  #endif

  int result = XGrabKeyboard(nxagentDisplay,
			     nxagentFullscreenWindow ? nxagentFullscreenWindow
			                             : RootWindow(nxagentDisplay, DefaultScreen(nxagentDisplay)),
			     True, GrabModeAsync, GrabModeAsync, now);

  if (result != GrabSuccess)
  {
    #ifdef DEBUG
    fprintf(stderr, "%s: keyboard grab failed.\n", __func__);
    #endif
    return;
  }
  #ifdef DEBUG
  else
  {
    fprintf(stderr, "%s: keyboard grab successful.\n", __func__);
  }
  #endif

  /*
   * The smart scheduler could be stopped while waiting for the
   * reply. In this case we need to yield explicitly to avoid to be
   * stuck in the dispatch loop forever.
   */

  isItTimeToYield = 1;

  #ifdef TEST
  fprintf(stderr, "%s: Going to grab the pointer in context [B2].\n", __func__);
  #endif

  int resource = nxagentWaitForResource(NXGetCollectGrabPointerResource,
                                            nxagentCollectGrabPointerPredicate);

  if (nxagentFullscreenWindow)
     NXCollectGrabPointer(nxagentDisplay, resource,
                           nxagentFullscreenWindow, True, NXAGENT_POINTER_EVENT_MASK,
                               GrabModeAsync, GrabModeAsync, None, None, now);

  /*
   * This should not be needed.
   *
   * XGrabKey(nxagentDisplay, AnyKey, AnyModifier, nxagentFullscreenWindow,
   *              True, GrabModeAsync, GrabModeAsync);
   */

  if (X != NULL)
  {
    #ifdef TEST
    fprintf(stderr, "%s: Going to force focus in context [B4].\n", __func__);
    #endif

    XSetInputFocus(nxagentDisplay, nxagentFullscreenWindow,
                       RevertToParent, now);
  }
}

void nxagentUngrabPointerAndKeyboard(XEvent *X)
{
  unsigned long now;

  #ifdef TEST
  fprintf(stderr, "%s: Ungrabbing pointer and keyboard with event at [%p].\n", __func__,
              (void *) X);
  #endif

  if (X != NULL)
  {
    now = X -> xcrossing.time;
  }
  else
  {
    now = CurrentTime;
  }

  #ifdef TEST
  fprintf(stderr, "%s: Going to ungrab the keyboard in context [B5].\n", __func__);
  #endif

  XUngrabKeyboard(nxagentDisplay, now);

  #ifdef TEST
  fprintf(stderr, "%s: Going to ungrab the pointer in context [B6].\n", __func__);
  #endif

  XUngrabPointer(nxagentDisplay, now);
}

void nxagentDeactivatePointerGrab(void)
{
  GrabPtr grab = inputInfo.pointer -> grab;

  if (grab)
  {
    XButtonEvent X = {
      .type = ButtonRelease,
      .serial = 0,
      .send_event = FALSE,
      .time = currentTime.milliseconds,
      .display = nxagentDisplay,
      .window = nxagentWindow(grab -> window),
      .root = RootWindow(nxagentDisplay, 0),
      .subwindow = 0,
      .x = 0,
      .y = 0,
      .x_root = 0,
      .y_root = 0,
      .state = 0x100,
      .button = 1,
      .same_screen = TRUE,
    };

    XPutBackEvent(nxagentDisplay, (XEvent*)&X);
  }
}

Bool nxagentCollectGrabPointerPredicate(Display *disp, XEvent *X, XPointer ptr)
{
  return (X -> xclient.window == 0 &&
             X -> xclient.message_type == 0 &&
                 X -> xclient.format == 32 &&
                     X -> xclient.data.l[0] == NXCollectGrabPointerNotify);
}

void nxagentHandleCollectGrabPointerEvent(int resource)
{
  int status;

  if (NXGetCollectedGrabPointer(nxagentDisplay, resource, &status) == 0)
  {
    #ifdef PANIC
    fprintf(stderr, "%s: PANIC! Failed to get GrabPointer reply for resource [%d].\n", __func__, resource);
    #endif
  }
}

void nxagentHandleCollectPropertyEvent(XEvent *X)
{
  int resource = X -> xclient.data.l[1];

  if (X -> xclient.data.l[2] == False)
  {
    #ifdef DEBUG
    fprintf (stderr, "%s: Failed to get reply data for client [%d].\n", __func__,
                 resource);
    #endif

    return;
  }

  if (!nxagentCollectPropertyEventFromXServer(resource))
  {
    XlibAtom atomReturnType;
    int resultFormat;
    unsigned long ulReturnItems;
    unsigned long ulReturnBytesLeft;
    unsigned char *pszReturnData = NULL;

    int result = NXGetCollectedProperty(nxagentDisplay,
                                        resource,
                                        &atomReturnType,
                                        &resultFormat,
                                        &ulReturnItems,
                                        &ulReturnBytesLeft,
                                        &pszReturnData);

    if (result == True)
    {
      XlibWindow window = nxagentPropertyRequests[resource].window;
      XlibAtom property = nxagentPropertyRequests[resource].property;

      nxagentImportProperty(window, property, atomReturnType, resultFormat,
                                ulReturnItems, ulReturnBytesLeft, pszReturnData);
    }
    else
    {
      #ifdef DEBUG
      fprintf (stderr, "%s: Failed to get reply data for client [%d].\n", __func__,
                   resource);
      #endif
    }

    SAFE_XFree(pszReturnData);

    return;
  }
}

void nxagentSynchronizeExpose(void)
{
  if (nxagentExposeQueue.length <= 0)
  {
    #ifdef TEST
    fprintf(stderr, "%s: PANIC! Called with nxagentExposeQueue.length [%d].\n", __func__,
                nxagentExposeQueue.length);
    #endif

    return;
  }

  WindowPtr pWin = nxagentExposeQueueHead.pWindow;

  if (pWin)
  {
    if ((nxagentExposeQueueHead.localRegion) != NullRegion)
    {
      RegionTranslate((nxagentExposeQueueHead.localRegion),
                           pWin -> drawable.x, pWin -> drawable.y);
    }

    if ((nxagentExposeQueueHead.remoteRegion) != NullRegion)
    {
      RegionTranslate((nxagentExposeQueueHead.remoteRegion),
                           pWin -> drawable.x, pWin -> drawable.y);
    }

    if ((nxagentExposeQueueHead.localRegion) != NullRegion &&
             (nxagentExposeQueueHead.remoteRegion) != NullRegion)
    {
      RegionSubtract((nxagentExposeQueueHead.remoteRegion),
                          (nxagentExposeQueueHead.remoteRegion),
                              (nxagentExposeQueueHead.localRegion));

      if (!RegionNil(nxagentExposeQueueHead.remoteRegion) &&
             ((pWin -> eventMask|wOtherEventMasks(pWin)) & ExposureMask))
      {
        #ifdef TEST
        fprintf(stderr, "%s: Going to call miWindowExposures for window [%d] - rects [%d].\n",
                    __func__, nxagentWindow(pWin),
                        RegionNumRects(nxagentExposeQueueHead.remoteRegion));
        #endif

        miWindowExposures(pWin, nxagentExposeQueueHead.remoteRegion, NullRegion);
      }
    }
  }

  nxagentExposeQueueHead.pWindow = NULL;

  if (nxagentExposeQueueHead.localRegion != NullRegion)
  {
    RegionDestroy(nxagentExposeQueueHead.localRegion);
  }
  nxagentExposeQueueHead.localRegion = NullRegion;

  if (nxagentExposeQueueHead.remoteRegion != NullRegion)
  {
    RegionDestroy(nxagentExposeQueueHead.remoteRegion);
  }
  nxagentExposeQueueHead.remoteRegion = NullRegion;
  nxagentExposeQueueHead.remoteRegionIsCompleted = False;

  nxagentExposeQueue.start = (nxagentExposeQueue.start + 1) % EXPOSED_SIZE;

  nxagentExposeQueue.length--;

  return;
}

int nxagentLookupByWindow(WindowPtr pWin)
{
  for (int j = 0; j < nxagentExposeQueue.length; j++)
  {
    int i = (nxagentExposeQueue.start + j) % EXPOSED_SIZE;

    if (nxagentExposeQueue.exposures[i].pWindow == pWin &&
            !nxagentExposeQueue.exposures[i].remoteRegionIsCompleted)
    {
      return i;
    }
  }

  return -1;
}

void nxagentRemoveDuplicatedKeys(XEvent *X)
{
  _XQEvent *qelt = nxagentDisplay -> head;

  KeyCode lastKeycode = X -> xkey.keycode;

  if (qelt == NULL)
  {
    #ifdef TEST

    fprintf(stderr, "%s: Trying to read more events from the X server.\n", __func__);

    if (nxagentReadEvents(nxagentDisplay) > 0)
    {
      fprintf(stderr, "%s: Successfully read more events from the X server.\n", __func__);
    }

    #else

    nxagentReadEvents(nxagentDisplay);

    #endif

    qelt = nxagentDisplay -> head;
  }

  if (qelt != NULL)
  {
    _XQEvent *prev;
    _XQEvent *qeltKeyRelease;
    _XQEvent *prevKeyRelease;

    prev = qeltKeyRelease = prevKeyRelease = NULL;

    LockDisplay(nxagentDisplay);

    while (qelt != NULL)
    {
      if (qelt -> event.type == KeyRelease ||
              qelt -> event.type == KeyPress)
      {
        if (qelt -> event.xkey.keycode != lastKeycode ||
               (qelt -> event.type == KeyPress && qeltKeyRelease == NULL) ||
                   (qelt -> event.type == KeyRelease && qeltKeyRelease != NULL))
        {
          break;
        }

        if (qelt -> event.type == KeyRelease)
        {
          prevKeyRelease = prev;

          qeltKeyRelease = qelt;
        }
        else if (qelt -> event.type == KeyPress)
        {
          _XDeq(nxagentDisplay, prev, qelt);

          qelt = prev -> next;

          if (prev == qeltKeyRelease)
          {
            prev = prevKeyRelease;
          }

          _XDeq(nxagentDisplay, prevKeyRelease, qeltKeyRelease);

          qeltKeyRelease = prevKeyRelease = NULL;

          continue;
        }
      }

      prev = qelt;

      qelt = qelt -> next;
    }

    UnlockDisplay(nxagentDisplay);
  }
}

void nxagentInitRemoteExposeRegion(void)
{
  if (nxagentRemoteExposeRegion == NULL)
  {
    nxagentRemoteExposeRegion = RegionCreate(NULL, 1);

    if (nxagentRemoteExposeRegion == NULL)
    {
      #ifdef PANIC
      fprintf(stderr, "%s: PANIC! Failed to create expose region.\n", __func__);
      #endif
    }
  }
}

void nxagentForwardRemoteExpose(void)
{
  if (RegionNotEmpty(nxagentRemoteExposeRegion))
  {
    #ifdef DEBUG
    fprintf(stderr, "%s: Going to forward events.\n", __func__);
    #endif

    TraverseTree(screenInfo.screens[0]->root, nxagentClipAndSendExpose, (void *)nxagentRemoteExposeRegion);

    /*
     * Now this region should be empty.
     */

    RegionEmpty(nxagentRemoteExposeRegion);
  }
}

void nxagentAddRectToRemoteExposeRegion(BoxPtr rect)
{
  if (nxagentRemoteExposeRegion == NULL)
  {
    return;
  }

  RegionRec exposeRegion;
  RegionInit(&exposeRegion, rect, 1);

  RegionUnion(nxagentRemoteExposeRegion,
                   nxagentRemoteExposeRegion, &exposeRegion);

  RegionUninit(&exposeRegion);
}

int nxagentClipAndSendExpose(WindowPtr pWin, void * ptr)
{
  RegionPtr remoteExposeRgn = (RegionRec *) ptr;

  #ifdef DEBUG
  fprintf(stderr, "%s: Called.\n", __func__);
  #endif

  if (pWin -> drawable.class != InputOnly)
  {
    RegionPtr exposeRgn = RegionCreate(NULL, 1);

    #ifdef DEBUG
    BoxRec box = *RegionExtents(remoteExposeRgn);

    fprintf(stderr, "%s: Root expose extents: [%d] [%d] [%d] [%d].\n", __func__,
                box.x1, box.y1, box.x2, box.y2);

    box = *RegionExtents(&pWin -> clipList);

    fprintf(stderr, "%s: Clip list extents for window at [%p]: [%d] [%d] [%d] [%d].\n", __func__,
                (void *)pWin, box.x1, box.y1, box.x2, box.y2);
    #endif

    RegionIntersect(exposeRgn, remoteExposeRgn, &pWin -> clipList);

    if (RegionNotEmpty(exposeRgn))
    {
      #ifdef DEBUG
      fprintf(stderr, "%s: Forwarding expose to window at [%p] pWin.\n", __func__,
                  (void *)pWin);
      #endif

      /*
       * The miWindowExposures() clears out the region parameters, so
       * the subtract ope- ration must be done before calling it.
       */

      RegionSubtract(remoteExposeRgn, remoteExposeRgn, exposeRgn);

      miWindowExposures(pWin, exposeRgn, NullRegion);
    }

    RegionDestroy(exposeRgn);
  }

  if (RegionNotEmpty(remoteExposeRgn))
  {
    #ifdef DEBUG
    fprintf(stderr, "%s: Region not empty. Walk children.\n", __func__);
    #endif

    return WT_WALKCHILDREN;
  }
  else
  {
    #ifdef DEBUG
    fprintf(stderr, "%s: Region empty. Stop walking.\n", __func__);
    #endif

    return WT_STOPWALKING;
  }
}

int nxagentUserInput(void *p)
{
  int result = 0;

  /*
   * This function is used as callback in the polling handler of agent
   * in shadow mode. When inside the polling loop the handlers are
   * never called, so we have to dispatch enqueued events to
   * eventually change the nxagentInputEvent status.
   */

  if (nxagentOption(Shadow) &&
          nxagentPendingEvents(nxagentDisplay) > 0)
  {
    nxagentDispatchEvents(NULL);
  }

  if (nxagentInputEvent == 1)
  {
    nxagentInputEvent = 0;

    result = 1;
  }

  /*
   * The agent working in shadow mode synchronizes the remote X server
   * even if a button/key is not released (i.e. when scrolling a long
   * browser's page), in order to update the screen smoothly.
   */

  if (nxagentOption(Shadow))
  {
    return result;
  }

  if (result == 0)
  {
    /*
     * If there is at least one button/key down, we are receiving an
     * input. This is not a condition to break a synchronization loop
     * if there is enough bandwidth.
     */

    if (nxagentCongestion > 0 &&
            (inputInfo.pointer -> button -> buttonsDown > 0 ||
                nxagentKeyDown > 0))
    {
      #ifdef TEST
      fprintf(stderr, "%s: Buttons [%d] Keys [%d].\n", __func__,
                  inputInfo.pointer -> button -> buttonsDown, nxagentKeyDown);
      #endif

      result = 1;
    }
  }

  return result;
}

int nxagentHandleRRScreenChangeNotify(XEvent *X)
{
  XRRScreenChangeNotifyEvent *Xr = (XRRScreenChangeNotifyEvent *) X;

  #ifdef DEBUG
  fprintf(stderr, "%s: Called.\n", __func__);
  #endif

  nxagentResizeScreen(screenInfo.screens[DefaultScreen(nxagentDisplay)],
                          Xr -> width, Xr -> height,
                              Xr -> mwidth, Xr -> mheight, True);

  nxagentShadowCreateMainWindow(screenInfo.screens[DefaultScreen(nxagentDisplay)],
                                    screenInfo.screens[0]->root,
                                        Xr -> width, Xr -> height);

  nxagentShadowSetWindowsSize();

  return 1;
}

/*
 * Returns true if there is any event waiting to be dispatched. This
 * function is critical for the performance because it is called very,
 * very often. It must also handle the case when the display is
 * down. The display descriptor, in fact, may have been reused by some
 * other client.
 */

int nxagentPendingEvents(Display *dpy)
{
  if (_XGetIOError(dpy) != 0)
  {
    #ifdef DEBUG
    fprintf(stderr, "%s: Returning error with display down.\n", __func__);
    #endif

    return -1;
  }
  else if (XQLength(dpy) > 0)
  {
    #ifdef DEBUG
    fprintf(stderr, "%s: Returning true with [%d] events queued.\n", __func__,
                XQLength(dpy));
    #endif

    return 1;
  }
  else
  {
    int readable;

    if (NXTransReadable(dpy -> fd, &readable) == 0)
    {
      if (readable > 0)
      {
        #ifdef DEBUG
        fprintf(stderr, "%s: Returning true with [%d] bytes readable.\n", __func__,
                    readable);
        #endif

        return 1;
      }

      #ifdef DEBUG
      fprintf(stderr, "%s: Returning false with [%d] bytes readable.\n", __func__,
                  readable);
      #endif

      return 0;
    }

    #ifdef TEST
    fprintf(stderr, "%s: WARNING! Error detected on the X display.\n", __func__);
    #endif

    NXForceDisplayError(dpy);

    return -1;
  }
}

/*
 * Blocks until an event becomes available.
 */

int nxagentWaitEvents(Display *dpy, useconds_t msec)
{
  #ifdef DEBUG
  fprintf(stderr, "%s: Called.\n", __func__);
  #endif

  NXFlushDisplay(dpy, NXFlushLink);

  /*
   * If the transport is not running we have to rely on Xlib to wait
   * for an event. In this case the timeout is ignored.
   */

  if (NXTransRunning(NX_FD_ANY) == 1)
  {
    if (msec > 0)
    {
      struct timeval tm = {
	  .tv_sec  = 0,
	  .tv_usec = msec * 1000
      };
      NXTransContinue(&tm);
    }
    else
    {
      NXTransContinue(NULL);
    }
  }
  else
  {
    XEvent ev;
    XPeekEvent(dpy, &ev);
  }

  /*
   * Check if we encountered a display error. If we did, wait for the
   * time requested by the caller.
   */

  if (NXDisplayError(dpy) == 1)
  {
    if (msec > 0)
    {
      usleep(msec * 1000);
    }

    return -1;
  }

  return 1;
}

void ForwardClientMessage(ClientPtr client, xSendEventReq *stuff)
{
    Atom netwmstate = MakeAtom("_NET_WM_STATE", strlen("_NET_WM_STATE"), False);
    Atom wmchangestate = MakeAtom("WM_CHANGE_STATE", strlen("WM_CHANGE_STATE"), False);
    WindowPtr pWin = (WindowPtr)SecurityLookupWindow(stuff->destination, client,
                                                     DixReadAccess);

    if (stuff->event.u.clientMessage.u.l.type == netwmstate || stuff->event.u.clientMessage.u.l.type == wmchangestate)
    {
        if (pWin->drawable.id == pWin->drawable.pScreen->root->drawable.id)
        {
            #ifdef DEBUG
            fprintf(stderr, "%s: dest [0x%x] window [0x%x] clmsg.type [%d]->[%d]\n", __func__, stuff->destination, stuff->event.u.clientMessage.window, stuff->event.u.clientMessage.u.l.type, nxagentLocalToRemoteAtom(stuff->event.u.clientMessage.u.l.type));
            #endif

            XEvent X = {0};
            X.xany.type = ClientMessage;

            WindowPtr pWin2 = (WindowPtr)SecurityLookupWindow(stuff->event.u.clientMessage.window, client,
                                                              DixReadAccess);
            X.xclient.window = nxagentWindowPriv(pWin2)->window;
            X.xclient.format = stuff->event.u.u.detail;
            X.xclient.send_event = True;
            X.xclient.serial = 0;

            if (X.xclient.format == 32)
            {
                X.xclient.message_type = nxagentLocalToRemoteAtom(stuff->event.u.clientMessage.u.l.type);
                X.xclient.data.l[0] = stuff->event.u.clientMessage.u.l.longs0;
                X.xclient.data.l[1] = nxagentLocalToRemoteAtom(stuff->event.u.clientMessage.u.l.longs1);
                X.xclient.data.l[2] = nxagentLocalToRemoteAtom(stuff->event.u.clientMessage.u.l.longs2);
                X.xclient.data.l[3] = nxagentLocalToRemoteAtom(stuff->event.u.clientMessage.u.l.longs3);
                X.xclient.data.l[4] = nxagentLocalToRemoteAtom(stuff->event.u.clientMessage.u.l.longs4);
                //X.xclient.data.l[3] = stuff->event.u.clientMessage.u.l.longs3;
                //X.xclient.data.l[4] = stuff->event.u.clientMessage.u.l.longs4;
                #ifdef DEBUG
                for (int i = 0; i < 5; i++)
                {
                    fprintf(stderr, "%s: data[%d] [%ld]\n", __func__, i, X.xclient.data.l[i]);
                }
                #endif
            }
            else
                return; // ERROR!

            #ifdef DEBUG
            fprintf(stderr, "%s: window [0x%lx]\n", __func__, X.xclient.window);
            fprintf(stderr, "%s: message_type [%ld]\n", __func__, X.xclient.message_type);
            fprintf(stderr, "%s: format [%d]\n", __func__, X.xclient.format);
            #endif

            XlibWindow dest;
            dest = DefaultRootWindow(nxagentDisplay);

            #ifdef DEBUG
            Status stat =
            #endif
            XSendEvent(nxagentDisplay, dest, stuff->propagate, stuff->eventMask, &X);
            XFlush(nxagentDisplay);
            #ifdef DEBUG
            fprintf(stderr, "%s: send to window [0x%lx]\n", __func__, dest);
            fprintf(stderr, "%s: return Status [%d]\n", __func__, stat);
            #endif
        }
    }
}

#ifdef NX_DEBUG_INPUT

void nxagentGuessDumpInputInfo(ClientPtr client, Atom property, char *data)
{
  if (strcmp(validateString(NameForAtom(property)), "NX_DEBUG_INPUT") == 0)
  {
    if (*data != 0)
    {
      nxagentDebugInput = 1;
    }
    else
    {
      nxagentDebugInput = 0;
    }
  }
}

void nxagentDeactivateInputDevicesGrabs(void)
{
  fprintf(stderr, "Info: Deactivating input devices grabs.\n");

  if (inputInfo.pointer -> grab)
  {
    (*inputInfo.pointer -> DeactivateGrab)(inputInfo.pointer);
  }

  if (inputInfo.keyboard -> grab)
  {
    (*inputInfo.keyboard -> DeactivateGrab)(inputInfo.keyboard);
  }
}

static const char *nxagentGrabStateToString(int state)
{
  switch (state)
  {
    case 0:      return "NOT_GRABBED";
    case 1:      return "THAWED";
    case 2:      return "THAWED_BOTH";
    case 3:      return "FREEZE_NEXT_EVENT";
    case 4:      return "FREEZE_BOTH_NEXT_EVENT";
    case 5:      return "FROZEN_NO_EVENT";
    case 6:      return "FROZEN_WITH_EVENT";
    case 7:      return "THAW_OTHERS";
    default:     return "unknown state";
  }
}

void nxagentDumpInputDevicesState(void)
{
  WindowPtr pWin = NULL;

  fprintf(stderr, "\n*** Dump input devices state: BEGIN ***"
              "\nKeys down:");

  DeviceIntPtr dev = inputInfo.keyboard;

  for (int i = 0; i < DOWN_LENGTH; i++)
  {
    CARD8 val = dev -> key -> down[i];

    if (val != 0)
    {
      for (int k = 0; k < 8; k++)
      {
        if (val & (1 << k))
        {
          fprintf(stderr, "\n\t[%d] [%s]", i * 8 + k,
                      XKeysymToString(XKeycodeToKeysym(nxagentDisplay, i * 8 + k, 0)));
        }
      }
    }
  }

  fprintf(stderr, "\nKeyboard device state: \n\tdevice [%p]\n\tlast grab time [%u]"
              "\n\tfrozen [%s]\n\tstate [%s]\n\tother [%p]\n\tevent count [%d]"
                  "\n\tfrom passive grab [%s]\n\tactivating key [%d]", (void *)dev,
                      dev -> grabTime.milliseconds, dev -> sync.frozen ? "Yes": "No",
                          nxagentGrabStateToString(dev -> sync.state),
                              (void *)dev -> sync.other, dev -> sync.evcount,
                                  dev -> fromPassiveGrab ? "Yes" : "No",
                                      dev -> activatingKey);

  GrabPtr grab = dev -> grab;

  if (grab)
  {
    fprintf(stderr, "\nKeyboard grab state: \n\twindow pointer [%p]"
                "\n\towner events flag [%s]\n\tgrab mode [%s]",
                    (void *)grab -> window, grab -> ownerEvents ? "True" : "False",
                        grab -> keyboardMode ? "asynchronous" : "synchronous");

   /*
    * Passive grabs.
    */

    pWin = grab -> window;
    grab = wPassiveGrabs(pWin);

    while (grab)
    {
      fprintf(stderr, "\nPassive grab state: \n\tdevice [%p]\n\towner events flag [%s]"
                  "\n\tpointer grab mode [%s]\n\tkeyboard grab mode [%s]\n\tevent type [%d]"
                      "\n\tmodifiers [%x]\n\tbutton/key [%u]\n\tevent mask [%x]",
                          (void *)grab -> device, grab -> ownerEvents ? "True" : "False",
                              grab -> pointerMode ? "asynchronous" : "synchronous",
                                  grab -> keyboardMode ? "asynchronous" : "synchronous",
                                      grab -> type, grab -> modifiersDetail.exact,
                                          grab -> detail.exact, grab -> eventMask);

      grab = grab -> next;
    }
  }

  fprintf(stderr, "\nButtons down:");

  dev = inputInfo.pointer;

  for (int i = 0; i < DOWN_LENGTH; i++)
  {
    CARD8 val = dev -> button -> down[i];

    if (val != 0)
    {
      for (int k = 0; k < 8; k++)
      {
        if (val & (1 << k))
        {
          fprintf(stderr, "\n\t[%d]", i * 8 + k);
        }
      }
    }
  }

  fprintf(stderr, "\nPointer device state: \n\tdevice [%p]\n\tlast grab time [%u]"
              "\n\tfrozen [%s]\n\tstate [%s]\n\tother [%p]\n\tevent count [%d]"
                  "\n\tfrom passive grab [%s]\n\tactivating button [%d]", (void *)dev,
                      dev -> grabTime.milliseconds, dev -> sync.frozen ? "Yes" : "No",
                          nxagentGrabStateToString(dev -> sync.state),
                              (void *)dev -> sync.other, dev -> sync.evcount,
                                  dev -> fromPassiveGrab ? "Yes" : "No",
                                      dev -> activatingKey);

  grab = dev -> grab;

  if (grab)
  {
    fprintf(stderr, "\nPointer grab state: \n\twindow pointer [%p]"
                "\n\towner events flag [%s]\n\tgrab mode [%s]",
                    (void *)grab -> window, grab -> ownerEvents ? "True" : "False",
                        grab -> pointerMode ? "asynchronous" : "synchronous");

    if (grab -> window != pWin)
    {
      /*
       * Passive grabs.
       */

      grab = wPassiveGrabs(grab -> window);

      while (grab)
      {
        fprintf(stderr, "\nPassive grab state: \n\tdevice [%p]\n\towner events flag [%s]"
                    "\n\tpointer grab mode [%s]\n\tkeyboard grab mode [%s]\n\tevent type [%d]"
                        "\n\tmodifiers [%x]\n\tbutton/key [%u]\n\tevent mask [%x]",
                            (void *)grab -> device, grab -> ownerEvents ? "True" : "False",
                                grab -> pointerMode ? "asynchronous" : "synchronous",
                                    grab -> keyboardMode ? "asynchronous" : "synchronous",
                                        grab -> type, grab -> modifiersDetail.exact,
                                            grab -> detail.exact, grab -> eventMask);

        grab = grab -> next;
      }
    }
  }

  fprintf(stderr, "\n*** Dump input devices state: FINISH ***\n");
}

#endif
