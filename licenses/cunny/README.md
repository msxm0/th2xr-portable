# CuNNy

`shaders/cunny/cunny-*-nvl.bin` hold the weights of CuNNy's NVL models
(fast, 4x16, 4x32), extracted by `tools/cunny_convert.py` from the Magpie
effects `magpie/normal/CuNNy-<model>-NVL.hlsl` of
https://github.com/funnyplanter/CuNNy at commit
906031bb00c15dd6a6bbbaa21c0eb0b724ca8437.  `src/cunny_model.cpp` turns them
back into shaders at run time.

CuNNy is licensed under the GNU Lesser General Public License v3
(`LGPL-3.0.txt`), whose terms are additional permissions on the GNU General
Public License v3 (`GPL-3.0.txt`); the generated Magpie effects themselves
carry a GPL v3-or-later header.  A build that includes these weights is
therefore distributed under the GPL v3 - see the License section of the
top-level README.
