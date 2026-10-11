// The pod's own web pages' palette (webapi.cpp), so the app and the fallback
// pages look like one thing. Dark only: it is used in a car.
export const C = {
  bg: '#0d1116',
  card: '#151b22',
  border: '#26313C',
  text: '#e7edf3',
  dim: '#8a97a3',
  accent: '#E8A33D',
  link: '#17A9E6',
  bad: '#E5544B',
  good: '#4CC38A',
} as const;

export const S = { xs: 4, sm: 8, md: 16, lg: 24 } as const;
