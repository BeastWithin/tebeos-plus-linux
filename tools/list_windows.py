import sys
from Xlib import display
d = display.Display(':0')
root = d.screen().root
for c in root.query_tree().children:
    try: name=c.get_wm_name(); cls=c.get_wm_class()
    except: name=None; cls=None
    try: g=c.get_geometry(); gd=(g.x,g.y,g.width,g.height)
    except: gd=None
    if name or (cls and cls[1]):
        print('  name=%r cls=%r geo=%s' % (name, cls, gd))
