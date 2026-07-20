# 这版无法在游戏内工作因为cs2默认获取鼠标原始输入
# This version cannot work within the game because CS2 obtains raw mouse input by default
# deSubtickForAttacks
delete cs2's stupid subtick for attacks in a stupid way
## 概述
如题，本程序用于去除按左键开枪时cs2的subtick，原理很bt，就是用一个程序劫持鼠标左键，在每一tick（硬编码为1/64s）结束时再向系统输入你的鼠标状态，以达到手感上先按左键后瞄准的csgo手感。
## 免责声明
许可证文件中有，这里特别强调，使用本程序产生包括steam账号vac但不限于的任何负面影响，本人概不负责。使用即视为同意许可证中的条款。
## 人工智能声明
本程序完全由DeepSeek网页版（2026/7/19）编写。

## Overview
As the title suggests, this program is designed to eliminate the subtick of CS2 when clicking the left button to fire. The principle is quite ingenious: it hijacks the mouse left button with a program and inputs your mouse state to the system at the end of each tick (hardcoded as 1/64s), achieving a CSGO-like feel where you click the left button first and then aim.
## Disclaimers
The license file states, and I emphasize here, that I am not responsible for any negative impacts, including but not limited to Steam account VAC, arising from the use of this program. Use of this program constitutes agreement to the terms stated in the license.
## AI Declaration
This program is completely written by DeepSeek Web Version (2026/7/19).
