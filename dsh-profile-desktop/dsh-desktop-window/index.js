/**
 * dsh-desktop-window: spawn the GTK WebKit webview window pointed at the
 * composed web URL, kill it when the plugin disposes, and (by default) exit
 * the dsh process when the window closes — desktop-app lifecycle semantics.
 */
import { spawn } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'

export const name = 'desktop-window'

export function apply(ctx, config) {
  const url = (config && config.url) || 'http://127.0.0.1:3080'
  const exitOnClose = !config || config.exitOnClose !== false
  const script = join(dirname(fileURLToPath(import.meta.url)), 'window.py')

  ctx.effect(() => {
    const child = spawn('python3', [script, url], { stdio: 'inherit' })
    child.on('error', (err) => {
      console.error('[desktop-window] failed to spawn webview:', err.message)
    })
    const onExit = (code) => {
      if (exitOnClose) {
        console.error('[desktop-window] window closed (code ' + code + '), shutting down')
        process.exit(0)
      }
    }
    child.on('exit', onExit)
    return () => {
      child.off('exit', onExit)
      try {
        child.kill()
      } catch {
        // already gone
      }
    }
  })
}
