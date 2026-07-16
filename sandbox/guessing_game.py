#!/usr/bin/env python3
"""
数字猜谜游戏 - Number Guessing Game
计算机随机生成一个数字，玩家来猜。
"""

import random


def guessing_game():
    print("🎯 欢迎来到数字猜谜游戏！")
    print("=" * 40)
    print("我已经想好了一个 1~100 之间的整数。")
    
    secret = random.randint(1, 100)
    attempts = 0
    
    while True:
        try:
            guess = input("\n请输入你的猜测 (输入 q 退出): ")
            if guess.lower() == 'q':
                print(f"👋 再见！秘密数字是 {secret}")
                break
            
            guess = int(guess)
            attempts += 1
            
            if guess < 1 or guess > 100:
                print("⚠️  请输入 1~100 之间的数字！")
                continue
            
            if guess < secret:
                print("📈 太小了，再大一点！")
            elif guess > secret:
                print("📉 太大了，再小一点！")
            else:
                print(f"\n🎉 恭喜你猜对了！数字就是 {secret}")
                print(f"🏆 你一共猜了 {attempts} 次")
                
                if attempts <= 3:
                    print("🌟 太厉害了！你是天才！")
                elif attempts <= 7:
                    print("👍 不错嘛，水平还可以！")
                else:
                    print("😅 还行，再接再厉！")
                break
                
        except ValueError:
            print("❌ 请输入有效的整数！")
        except KeyboardInterrupt:
            print("\n👋 下次再玩吧！")
            break


if __name__ == "__main__":
    guessing_game()
