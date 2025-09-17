TOP=10
DIR=testdata
CC=clang-20

# .PHONY: testdata
testdata:
	@echo "Create test data"
	mkdir -p $(DIR)
	# Manual constant to have ~1MB input file
	python3 generate.py 30000 | shuf > $(DIR)/test.1m.in

	# Manual constant to have ~1GB input file
	python3 generate.py 25400000 | shuf > $(DIR)/test.1g.in

	# Manual constant to have ~10GB input file
	python3 generate.py 254000000 | shuf > testdata/test.10g.in

.PHONY: evict
evict:
	@echo "Evict testing data from Page Cache for clean measurements\n"
	vmtouch -e $(DIR)/*.in

.PHONY: boring
boring: evict
	@echo "For real tasks there is no sense to program at all, we can solve it with boring unix tools"
	@echo "I'll use it to check correctness of my solution"

	@echo "\n\n💤Getting top $(TOP) lines from 1m file\n"
	time  sort -k2 -n -r $(DIR)/test.1m.in | cut -d ' ' -f1 | head -$(TOP) > $(DIR)/test.1m.out
	cat $(DIR)/test.1m.out

	@echo "\n\n💤Getting top $(TOP) lines from 1g file\n"
	time sort -k2 -n -r $(DIR)/test.1g.in | cut -d ' ' -f1 | head -$(TOP) > $(DIR)/test.1g.out
	cat $(DIR)/test.1g.out

	@echo "\n\n💤Getting top $(TOP) lines from 10g file\n"
	time sort -k2 -n -r $(DIR)/test.10g.in | cut -d ' ' -f1 | head -$(TOP) > $(DIR)/test.10g.out
	cat $(DIR)/test.10g.out

.PHONY: sane
sane: evict
	@echo "This is expected solution to discuss. I think it's overkill already but why not"
	go build sane.go

	@echo "\n\n💤Getting top $(TOP) lines from 1m file\n"
	echo "$(DIR)/test.1m.in" | time ./sane > $(DIR)/sane.1m.out
	cat $(DIR)/sane.1m.out

	@echo "\n\n💤Getting top $(TOP) lines from 1g file\n"
	echo "$(DIR)/test.1g.in" | time ./sane > $(DIR)/sane.1g.out
	cat $(DIR)/sane.1g.out

	@echo "\n\n💤Getting top $(TOP) lines from 10g file\n"
	echo "$(DIR)/test.10g.in" | time ./sane > $(DIR)/sane.10g.out
	cat $(DIR)/sane.10g.out

.PHONY: insane
insane: evict
	@echo "❗This is my insane solution to discuss. 🤪"
	@echo "❗There is no reason on earth to do this way but LET'S HAVE FUN 🤪\n"
	$(CC) -O2 -march=native -g  insane.c -o insane

	@echo "\n\n💤Getting top $(TOP) lines from 1m file\n"
	echo "$(DIR)/test.1m.in" | time ./insane > $(DIR)/insane.1m.out
	cat $(DIR)/insane.1m.out

	@echo "\n\n💤Getting top $(TOP) lines from 1g file\n"
	echo "$(DIR)/test.1g.in" | time ./insane > $(DIR)/insane.1g.out
	cat $(DIR)/insane.1g.out

	@echo "\n\n💤Getting top $(TOP) lines from 10g file\n"
	echo "$(DIR)/test.10g.in" | time ./insane > $(DIR)/insane.10g.out
	cat $(DIR)/insane.10g.out


test:
	@echo "Check correctness of sane solution"

	# I found that I cannot use diff because sort order for URL with same count is not guaranteed
# 	diff $(DIR)/test.1m.out $(DIR)/sane.1m.out >> /dev/null && echo "✅ Passed" || echo "⛔ Failed, outputs differ"
# 	diff $(DIR)/test.1g.out $(DIR)/sane.1g.out >> /dev/null && echo "✅ Passed" || echo "⛔ Failed, outputs differ"
