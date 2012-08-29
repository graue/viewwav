viewwav
=======

Very basic viewer for raw 16-bit, stereo wave files. I use it by piping
the output of a command that produces raw audio to `viewwav -`, or, if
the sample rate is not 44100 Hz, something like `RATE=48000 viewwav -`.
Somewhat embarrassingly, this program does not have an actual GUI (it
uses the Allegro game library) and cannot play or edit audio, only view
it. It is however pretty good at revealing the overall dynamics of a
song. And it's fast.

![[screenshot]](screenshot-linear.png)

Screenshot viewing the track "Burning Wheel" by Primal Scream. Green is
peak level, cyan is an
[https://en.wikipedia.org/wiki/Root_mean_square](RMS average) that
better reflects perceived loudness. The RMS display is off by default
and is toggled by pressing 'r'.

![[screenshot]](screenshot-log.png)

Same track in logarithmic view, toggled by pressing 'l'. The distance
between adjacent grey lines is 6dB. Here the cyan-colored RMS trace
provides a good overview of the dynamics in a song (with the exception
of very bass-heavy tracks where this can be deceiving).

Keys:
* `l`: Toggle linear/logarithmic view
* `p`: Toggle display of peak level
* `r`: Toggle display of RMS level
* Up/Down: Zoom in/out
* Left/Right, PgUp/PgDn: Move backward/forward in time
* Home/End: Jump to beginning/end of waveform
* F4/F3: In linear view, zoom in/out vertically
* Esc: Quit


License
-------

Written by Scott Feeney <scott@oceanbase.org>, 2007-2008

This software and associated documentation files (the "Software") is
released under the CC0 Public Domain Dedication, version 1.0, as
published by Creative Commons. To the extent possible under law, the
author(s) have dedicated all copyright and related and neighboring
rights to the Software to the public domain worldwide. The Software is
distributed WITHOUT ANY WARRANTY.

If you did not receive a copy of the CC0 Public Domain Dedication
along with the Software, see
<http://creativecommons.org/publicdomain/zero/1.0/>
