package main

import (
	"bufio"
	"container/heap"
	"fmt"
	"os"
	"strconv"
	"strings"
)

type T struct {
	url   string
	count uint64
}

type THeap []T

func (h THeap) Len() int           { return len(h) }
func (h THeap) Less(i, j int) bool { return h[i].count < h[j].count }
func (h THeap) Swap(i, j int)      { h[i], h[j] = h[j], h[i] }

func (h *THeap) Push(x any) { *h = append(*h, x.(T)) }
func (h *THeap) Pop() any {
	old := *h
	n := len(old)
	x := old[n-1]
	*h = old[0 : n-1]
	return x
}

func do(s *bufio.Scanner, topK int) {
	h := &THeap{}
	heap.Init(h)

	for s.Scan() {
		line := s.Text()
		delimeter := strings.Index(line, " ")
		if delimeter == -1 {
			panic("malformed line: no space in line")
		}
		url := line[:delimeter]
		count, err := strconv.ParseUint(line[delimeter+1:], 10, 64)
		if err != nil {
			panic("malformed line: failed to parse 2nd column to int")
		}
		heap.Push(h, T{url: url, count: count})
		if h.Len() > topK {
			heap.Pop(h)
		}
	}

	// Because I used max-heap, I need to reverse the order
	// Let's create a stack, put everything onto it and then go backwards
	stack := make([]T, 0, h.Len())
	for h.Len() > 0 {
		it := heap.Pop(h).(T)
		stack = append(stack, it)
	}
	for i := len(stack) - 1; i >= 0; i-- {
		fmt.Println(stack[i].url)
	}
}

func main() {
	scanner := bufio.NewScanner(os.Stdin)
	if scanner.Scan() {
		path := scanner.Text()
		file, err := os.Open(path)
		if err != nil {
			fmt.Fprintln(os.Stderr, "error opening file:", err)
			panic("error file")
		}
		defer file.Close()

		scanner := bufio.NewScanner(file)

		do(scanner, 10)
	}
	if err := scanner.Err(); err != nil {
		panic("Expected to get abspath to file")
	}
}
