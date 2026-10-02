// Elysia comparison server (Bun). Mirrors benchmarks/comparison/elaina/server.cpp.
// Run: bun run server.ts  (PORT env or arg, default 3000)
import { Elysia } from 'elysia'

const port = Number(process.argv[2] ?? process.env.PORT ?? 3000)
const largeBody = 'x'.repeat(1024 * 1024)

const app = new Elysia()
  .get('/', 'Hello World')
  .get('/static', 'static')
  .get('/users/:id', ({ params }) => params.id)
  .get('/users/:id/posts/:postId', ({ params }) => `${params.id}/${params.postId}`)
  .get('/search', ({ query }) => `${(query as any).q ?? ''}:${(query as any).n ?? ''}`)
  .get('/headers', ({ headers }) => (headers as any)['x-test'] ?? 'missing')
  .get('/json', { id: 42, name: 'elaina', ok: true })
  .post('/json', ({ body }) => body)
  .post('/echo', ({ body }) => body as string)
  .get('/bad', ({ set }) => { set.status = 400; return 'Bad Request' })
  .get('/large', largeBody)
  .listen(port)

console.log(`elysia listening on :${port}`)
