CREATE TABLE lr (i INT, l INT[]);
INSERT INTO lr VALUES (1, [1, 2, 3]), (2, [4, 5]), (3, [6]);
SELECT i, list_reduce(l, lambda acc, x: acc + x + i), list_reduce(l, lambda acc, x: acc * x, 100), list_reduce(NULL, lambda acc, x: acc + x + i, 100) FROM lr ORDER BY i;
