.PHONY: all convergence submission clean

all: submission

convergence:
	$(MAKE) -C convergence

submission:
	$(MAKE) -C submission

clean:
	$(MAKE) -C convergence clean
	$(MAKE) -C submission clean
	rm -f *.tgz
