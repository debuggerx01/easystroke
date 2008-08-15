/*
 * Copyright (c) 2008, Thomas Jaeger <ThJaeger@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "grabber.h"
#include "main.h"
#include <X11/extensions/XTest.h>
#include <X11/Xutil.h>

bool no_xi = false;
Grabber *grabber = 0;

template <class X1, class X2> class BiMap {
	std::map<X1, X2> map1;
	std::map<X2, X1> map2;
public:
	void erase1(X1 x1) {
		typename std::map<X1, X2>::iterator i1 = map1.find(x1);
		if (i1 == map1.end())
			return;
		map2.erase(i1->second);
		map1.erase(i1->first);
	}
	void erase2(X2 x2) {
		typename std::map<X2, X1>::iterator i2 = map2.find(x2);
		if (i2 == map2.end())
			return;
		map1.erase(i2->second);
		map2.erase(i2->first);
	}
	void add(X1 x1, X2 x2) {
		erase1(x1);
		erase2(x2);
		map1[x1] = x2;
		map2[x2] = x1;
	}
	bool contains1(X1 x1) { return map1.find(x1) != map1.end(); }
	bool contains2(X2 x2) { return map2.find(x2) != map2.end(); }
	X2 find1(X1 x1) { return map1.find(x1)->second; }
	X1 find2(X2 x2) { return map2.find(x2)->second; }
};

Atom XAtom::operator*() {
	if (!atom)
		atom = XInternAtom(dpy, name, False);
	return atom;
}

BiMap<Window, Window> frame_win;
std::map<Window, Window> frame_child;
XAtom _NET_FRAME_WINDOW("_NET_FRAME_WINDOW");

extern Window get_window(Window w, Atom prop);

void get_frame(Window w) {
	Window frame = get_window(w, *_NET_FRAME_WINDOW);
	if (!frame)
		return;
	frame_win.add(frame, w);
}

Children::Children(Window w) : parent(w) {
	XSelectInput(dpy, parent, SubstructureNotifyMask);
	unsigned int n;
	Window dummyw1, dummyw2, *ch;
	XQueryTree(dpy, parent, &dummyw1, &dummyw2, &ch, &n);
	for (unsigned int i = 0; i < n; i++)
		add(ch[i]);
	XFree(ch);
}

bool Children::handle(XEvent &ev) {
	switch (ev.type) {
		case CreateNotify:
			if (ev.xcreatewindow.parent != parent)
				return false;
			add(ev.xcreatewindow.window);
			return true;
		case DestroyNotify:
			if (ev.xdestroywindow.event != parent)
				return false;
			destroy(ev.xdestroywindow.window);
			return true;
		case ReparentNotify:
			if (ev.xreparent.event != parent)
				return false;
			if (ev.xreparent.window == parent)
				return false;
			if (ev.xreparent.parent == parent)
				add(ev.xreparent.window);
			else
				remove(ev.xreparent.window);
			return true;
		case PropertyNotify:
			if (ev.xproperty.atom != *_NET_FRAME_WINDOW)
				return true;
			if (ev.xproperty.state == PropertyDelete)
				frame_win.erase1(ev.xproperty.window);
			if (ev.xproperty.state == PropertyNewValue)
				get_frame(ev.xproperty.window);
			return true;
		default:
			return false;
	}
}

void Children::add(Window w) {
	if (!w)
		return;

	XSelectInput(dpy, w, EnterWindowMask | PropertyChangeMask);
	get_frame(w);
}

void Children::remove(Window w) {
	XSelectInput(dpy, w, 0);
	destroy(w);
}

void Children::destroy(Window w) {
	frame_win.erase1(w);
	frame_win.erase2(w);
}

extern Source<bool> xinput_v, supports_pressure, supports_proximity;

const char *Grabber::state_name[6] = { "None", "Button", "All (Sync)", "All (Async)", "Scroll", "Select" };

extern Window get_window(Window w, Atom prop);

bool wm_running() {
	static XAtom _NET_SUPPORTING_WM_CHECK("_NET_SUPPORTING_WM_CHECK");
	Window w = get_window(ROOT, *_NET_SUPPORTING_WM_CHECK);
	return w && get_window(w, *_NET_SUPPORTING_WM_CHECK) == w;
}

Grabber::Grabber() : children(ROOT) {
	current = BUTTON;
	suspended = false;
	active = true;
	grabbed = NONE;
	xi_grabbed = false;
	proximity_selected = false;
	get_button();

	xinput = init_xi();
	if (xinput)
		select_proximity();

	cursor_scroll = XCreateFontCursor(dpy, XC_double_arrow);
	cursor_select = XCreateFontCursor(dpy, XC_crosshair);
}

Grabber::~Grabber() {
	XFreeCursor(dpy, cursor_scroll);
	XFreeCursor(dpy, cursor_select);
}

float rescaleValuatorAxis(int coord, int fmin, int fmax, int tmax) {
	if (fmin >= fmax)
		return coord;
	return ((float)(coord - fmin)) * (tmax + 1) / (fmax - fmin + 1);
}

extern "C" {
	extern int _XiGetDevicePresenceNotifyEvent(Display *);
}

bool Grabber::init_xi() {
	xi_devs_n = 0;
	button_events_n = 3;
	if (no_xi)
		return false;
	int nMajor, nFEV, nFER;
	if (!XQueryExtension(dpy,INAME,&nMajor,&nFEV,&nFER))
		return false;

	int i, n;
	XDeviceInfo *devs;
	devs = XListInputDevices(dpy, &n);
	if (!devs)
		return false;

	xi_devs = new XiDevice *[n];

	for (i=0; i<n; ++i) {
		XDeviceInfo *dev = devs + i;

		if (dev->use == IsXKeyboard || dev->use == IsXPointer)
			continue;

		XiDevice *xi_dev = new XiDevice;

		bool has_button = false;
		xi_dev->supports_pressure = false;
		XAnyClassPtr any = (XAnyClassPtr) (dev->inputclassinfo);
		for (int j = 0; j < dev->num_classes; j++) {
			if (any->c_class == ButtonClass)
				has_button = true;
			if (any->c_class == ValuatorClass) {
				XValuatorInfo *info = (XValuatorInfo *)any;
				if (info->num_axes >= 2) {
					xi_dev->min_x = info->axes[0].min_value;
					xi_dev->max_x = info->axes[0].max_value;
					xi_dev->min_y = info->axes[1].min_value;
					xi_dev->max_y = info->axes[1].max_value;
				}
				if (info->num_axes >= 3) {
					xi_dev->supports_pressure = true;
					xi_dev->pressure_min = info->axes[2].min_value;
					xi_dev->pressure_max = info->axes[2].max_value;
				}
			}
			any = (XAnyClassPtr) ((char *) any + any->length);
		}

		if (!has_button) {
			delete xi_dev;
			continue;
		}

		xi_dev->dev = XOpenDevice(dpy, dev->id);
		if (!xi_dev->dev) {
			printf("Opening Device %s failed.\n", dev->name);
			delete xi_dev;
			continue;
		}

		DeviceButtonPress(xi_dev->dev, xi_dev->event_type[DOWN], xi_dev->events[DOWN]);
		DeviceButtonRelease(xi_dev->dev, xi_dev->event_type[UP], xi_dev->events[UP]);
		DeviceButtonMotion(xi_dev->dev, xi_dev->event_type[BUTTON_MOTION], xi_dev->events[BUTTON_MOTION]);
		DeviceMotionNotify(xi_dev->dev, xi_dev->event_type[MOTION], xi_dev->events[MOTION]);

		ProximityIn(xi_dev->dev, xi_dev->event_type[PROX_IN], xi_dev->events[PROX_IN]);
		ProximityOut(xi_dev->dev, xi_dev->event_type[PROX_OUT], xi_dev->events[PROX_OUT]);
		xi_dev->supports_proximity = xi_dev->events[PROX_IN] && xi_dev->events[PROX_OUT];
		xi_dev->all_events_n = xi_dev->supports_proximity ? 6 : 4;

		xi_devs[xi_devs_n++] = xi_dev;

		if (verbosity >= 1)
			printf("Opened Device \"%s\" (%s proximity).\n", dev->name,
					xi_dev->supports_proximity ? "supports" : "does not support");
	}
	XFreeDeviceList(devs);

	// Macro not c++-safe
	// DevicePresence(dpy, event_presence, presence_class);
	event_presence = _XiGetDevicePresenceNotifyEvent(dpy);
	presence_class =  (0x10000 | _devicePresence);

	xinput_v.set(xi_devs_n);

	for (int i = 0; i < xi_devs_n; i++)
		if (xi_devs[i]->supports_pressure) {
			supports_pressure.set(true);
			break;
		}

	for (int i = 0; i < xi_devs_n; i++)
		if (xi_devs[i]->supports_proximity) {
			supports_proximity.set(true);
			break;
		}
	class NotifyProx : public In {
		virtual void notify() { grabber->select_proximity(); }
	};
	prefs.proximity.connect(new NotifyProx);

	return xi_devs_n;
}

Grabber::XiDevice *Grabber::get_xi_dev(XID id) {
	for (int i = 0; i < xi_devs_n; i++)
		if (xi_devs[i]->dev->device_id == id)
			return xi_devs[i];
	return 0;
}

bool Grabber::is_event(int type, EventType et) {
	if (!xinput)
		return false;
	for (int i = 0; i < xi_devs_n; i++)
		if (type == xi_devs[i]->event_type[et]) {
			return true;
		}
	return false;
}

unsigned int Grabber::get_device_button_state() {
	unsigned int mask = 0;
	for (int i = 0; i < xi_devs_n; i++) {
		XDeviceState *state = XQueryDeviceState(dpy, xi_devs[i]->dev);
		if (!state)
			continue;
		XInputClass *c = state->data;
		for (int j = 0; j < state->num_classes; j++) {
			if (c->c_class == ButtonClass) {
				XButtonState *b = (XButtonState *)c;
				mask |= b->buttons[0];
				mask |= ((unsigned int)b->buttons[1]) << 8;
				mask |= ((unsigned int)b->buttons[2]) << 16;
				mask |= ((unsigned int)b->buttons[3]) << 24;
			}
			c = (XInputClass *)((char *)c + c->length);
		}
		XFreeDeviceState(state);
	}

	return mask;
}

void Grabber::grab_xi(bool grab) {
	if (!xi_grabbed == !grab)
		return;
	xi_grabbed = grab;
	if (grab) {
		for (int i = 0; i < xi_devs_n; i++)
			for (std::map<guint, guint>::iterator j = buttons.begin(); j != buttons.end(); j++) {
				XGrabDeviceButton(dpy, xi_devs[i]->dev, j->first, j->second, NULL, ROOT, False,
							button_events_n, xi_devs[i]->events,
							GrabModeAsync, GrabModeAsync);
				XGrabDeviceButton(dpy, xi_devs[i]->dev, j->first, j->second ^ Mod2Mask, NULL, ROOT, False,
							button_events_n, xi_devs[i]->events,
							GrabModeAsync, GrabModeAsync);
			}

	} else for (int i = 0; i < xi_devs_n; i++)
		for (std::map<guint, guint>::iterator j = buttons.begin(); j != buttons.end(); j++) {
			XUngrabDeviceButton(dpy, xi_devs[i]->dev, j->first, j->second ^ Mod2Mask, NULL, ROOT);
			XUngrabDeviceButton(dpy, xi_devs[i]->dev, j->first, j->second, NULL, ROOT);
		}
}

void Grabber::grab_xi_devs(bool grab) {
	if (grab) {
		for (int i = 0; i < xi_devs_n; i++)
			if (XGrabDevice(dpy, xi_devs[i]->dev, ROOT, False,
						xi_devs[i]->all_events_n,
						xi_devs[i]->events, GrabModeAsync, GrabModeAsync, CurrentTime))
				throw GrabFailedException();
	} else
		for (int i = 0; i < xi_devs_n; i++)
			XUngrabDevice(dpy, xi_devs[i]->dev, CurrentTime);
}

extern bool in_proximity;
void Grabber::select_proximity() {
	if (!prefs.proximity.get() != !proximity_selected) {
		proximity_selected = !proximity_selected;
		if (!proximity_selected)
			in_proximity = false;
	}
	XEventClass evs[2*xi_devs_n+1];
	int n = 0;
	evs[n++] = presence_class;
		for (int i = 0; i < xi_devs_n; i++) {
			if (proximity_selected && xi_devs[i]->supports_proximity) {
				evs[n++] = xi_devs[i]->events[PROX_IN];
				evs[n++] = xi_devs[i]->events[PROX_OUT];
			}
		}
	// NB: Unselecting doesn't actually work!
	XSelectExtensionEvent(dpy, ROOT, evs, n);
}

void Grabber::set() {
	bool act = (current == NONE || current == BUTTON) ? active : true;
	grab_xi(act);
	State old = grabbed;
	grabbed = (!suspended && act) ? current : NONE;
	if (old == grabbed)
		return;
	if (verbosity >= 2)
		printf("grabbing: %s\n", state_name[grabbed]);

	if (old == BUTTON) {
		for (std::map<guint, guint>::iterator j = buttons.begin(); j != buttons.end(); j++) {
			XUngrabButton(dpy, j->first, j->second ^ Mod2Mask, ROOT);
			XUngrabButton(dpy, j->first, j->second, ROOT);
		}
		if (timing_workaround)
			XUngrabButton(dpy, 1, AnyModifier, ROOT);
	}
	if (old == ALL_SYNC)
		XUngrabButton(dpy, AnyButton, AnyModifier, ROOT);
	if (old == ALL_ASYNC)
		XUngrabButton(dpy, AnyButton, AnyModifier, ROOT);
	if (old == SCROLL || old == SELECT) {
		XUngrabPointer(dpy, CurrentTime);
	}
	if (grabbed == BUTTON) {
		for (std::map<guint, guint>::iterator j = buttons.begin(); j != buttons.end(); j++) {
			XGrabButton(dpy, j->first, j->second, ROOT, False,
					ButtonMotionMask | ButtonPressMask | ButtonReleaseMask,
					GrabModeSync, GrabModeAsync, None, None);
			XGrabButton(dpy, j->first, j->second ^ Mod2Mask, ROOT, False,
					ButtonMotionMask | ButtonPressMask | ButtonReleaseMask,
					GrabModeSync, GrabModeAsync, None, None);
		}
		timing_workaround = !is_grabbed(1) && prefs.timing_workaround.get();
		if (timing_workaround)
			XGrabButton(dpy, 1, AnyModifier, ROOT, False, ButtonMotionMask | ButtonPressMask | ButtonReleaseMask,
					GrabModeSync, GrabModeAsync, None, None);
	}
	if (grabbed == ALL_SYNC)
		if (!XGrabButton(dpy, AnyButton, AnyModifier, ROOT, False,
					ButtonPressMask, GrabModeSync, GrabModeAsync, None, None))
			throw GrabFailedException();
	if (grabbed == ALL_ASYNC)
		XGrabButton(dpy, AnyButton, AnyModifier, ROOT, True, ButtonPressMask,
				GrabModeAsync, GrabModeAsync, None, None);
	if (grabbed == SCROLL || grabbed == SELECT) {
		int i = 0;
		while (XGrabPointer(dpy, ROOT, False, PointerMotionMask|ButtonMotionMask|ButtonPressMask|ButtonReleaseMask,
					GrabModeAsync, GrabModeAsync, ROOT,
					grabbed == SCROLL ? cursor_scroll : cursor_select, CurrentTime) != GrabSuccess) {
			if (++i > 10)
				throw GrabFailedException();
			usleep(10000);
		}
	}
}

void Grabber::get_button() {
	buttons.clear();
	const ButtonInfo &bi = prefs.button.ref();
	buttons[bi.button] = bi.state;
}

void Grabber::fake_button(int b) {
	suspend();
	XTestFakeButtonEvent(dpy, b, True, CurrentTime);
	XTestFakeButtonEvent(dpy, b, False, CurrentTime);
	clear_mods();
	resume();
}

std::string Grabber::get_wm_class(Window w) {
	if (!w)
		return "(window manager frame)";
	XClassHint ch;
	if (!XGetClassHint(dpy, w, &ch))
		return "";
	std::string ans = ch.res_name;
	XFree(ch.res_name);
	XFree(ch.res_class);
	return ans;
}

// Fuck Xlib
bool has_wm_state(Window w) {
	static XAtom WM_STATE("WM_STATE");
	Atom actual_type_return;
	int actual_format_return;
	unsigned long nitems_return;
	unsigned long bytes_after_return;
	unsigned char *prop_return;
	if (Success != XGetWindowProperty(dpy, w, *WM_STATE, 0, 2, False,
				AnyPropertyType, &actual_type_return,
				&actual_format_return, &nitems_return,
				&bytes_after_return, &prop_return))
		return false;
	XFree(prop_return);
	return nitems_return;
}

Window find_wm_state(Window w) {
	if (!w)
		return w;
	if (has_wm_state(w))
		return w;
	Window found = None;
	unsigned int n;
	Window dummyw1, dummyw2, *ch;
	if (!XQueryTree(dpy, w, &dummyw1, &dummyw2, &ch, &n))
		return None;
	for (unsigned int i = 0; i != n; i++)
		if (has_wm_state(ch[i]))
			found = ch[i];
	XFree(ch);
	return found;
}

// sets w to 0 if the window is a frame
Window get_app_window(Window &w) {
	if (!w)
		return w;
	if (frame_win.contains1(w)) {
		Window w2 = frame_win.find1(w);
		w = 0;
		return w2;
	}
	std::map<Window, Window>::iterator i = frame_child.find(w);
	if (i != frame_child.end()) {
		Window w2 = i->second;
		if (w != w2)
			w = 0;
		return w2;
	}
	Window w2 = find_wm_state(w);
	if (w2) {
		frame_child[w] = w2;
		if (w2 != w) {
			w = w2;
			XSelectInput(dpy, w2, EnterWindowMask | LeaveWindowMask);
		}
		return w2;
	}
	if (verbosity >= 1)
		printf("Window 0x%lx does not have an associated top-level window\n", w);
	w = 0;
	return w;
}
