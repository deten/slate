# Notices

Slate is copyright (c) 2026 deten and is free software: you can
redistribute it and modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3 of
the License, or (at your option) any later version. See `LICENSE` for the
terms. It is distributed in the hope that it will be useful, but without
any warranty; without even the implied warranty of merchantability or
fitness for a particular purpose.

Slate is built with and ships alongside software from others, under their
own terms:

## Qt

Slate uses the Qt framework (Qt 6, the Core, Gui, Widgets, Pdf and
related modules) under the GNU Lesser General Public License version 3.
Qt is linked dynamically: the Qt libraries ship beside `Slate.exe` as
separate files and can be replaced with your own builds of the same
versions. Qt's source is available from https://www.qt.io/download and
https://code.qt.io. The text of the LGPL v3 is at
https://www.gnu.org/licenses/lgpl-3.0.html. Qt is copyright The Qt
Company Ltd and its contributors.

## doctest

The tests are written with doctest, copyright 2016-2023 Viktor Kirilov,
distributed under the MIT License. It is used only to build and run the
tests and is not part of the program that ships.

## ODA File Converter

Reading and writing DWG files uses the ODA File Converter from the Open
Design Alliance, which is not distributed with Slate. Users download it
themselves from https://www.opendesign.com/guestfiles/oda_file_converter
and place it beside `Slate.exe`, under the Open Design Alliance's own
terms.
